/*	$NetBSD: kbd.c,v 1.59 2024/09/08 09:36:48 rillig Exp $	*/

/*
 * Copyright (c) 1995 Leo Weppelman
 * Copyright (c) 1982, 1986, 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: kbd.c,v 1.59 2024/09/08 09:36:48 rillig Exp $");

#include "mouse.h"
#include "ite.h"
#include "wskbd.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/proc.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/signalvar.h>
#include <sys/syslog.h>
#include <sys/rndsource.h>

#include <dev/cons.h>
#include <machine/cpu.h>
#include <machine/iomap.h>
#include <machine/mfp.h>
#include <machine/acia.h>
#include <atari/dev/itevar.h>
#include <atari/dev/event_var.h>
#include <atari/dev/vuid_event.h>
#include <atari/dev/ym2149reg.h>
#include <atari/dev/kbdreg.h>
#include <atari/dev/kbdvar.h>
#include <atari/dev/kbdmap.h>
#include <atari/dev/msvar.h>

#if NWSKBD>0
#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>
#include <atari/dev/wskbdmap_atari.h>
#endif

/*
 * The ringbuffer is the interface between the hard and soft interrupt handler.
 * The hard interrupt runs straight from the MFP interrupt.
 */
#define KBD_RING_SIZE	256   /* Sz of input ring buffer, must be power of 2 */
#define KBD_RING_MASK	255   /* Modulo mask for above			     */

struct kbd_softc {
	int		sc_event_mode;	/* if 1, collect events,	*/
					/*   else pass to ite		*/
	struct evvar	sc_events;	/* event queue state		*/
	uint8_t		sc_soft_cs;	/* control-reg. copy		*/
	uint8_t		sc_package[20];	/* XXX package being build	*/
	uint8_t		sc_pkg_size;	/* Size of the package		*/
	uint8_t		sc_pkg_idx;	/* Running pkg assembly index	*/
	uint8_t		sc_pkg_type;	/* Type of package		*/
	const uint8_t	*sc_sendp;	/* Output pointer		*/
	int		sc_send_cnt;	/* Chars left for output	*/
#if NWSKBD > 0
	device_t	sc_wskbddev;  /* pointer to wskbd for sending strokes */
	int		sc_pollingmode;	/* polling mode on? whatever it is... */
#endif
	void		*sc_sicookie;	/* softint(9) cookie		*/
	krndsource_t	sc_rndsource;	/* rnd(9) entropy		*/
};

/* WSKBD */
/*
 * If NWSKBD>0 we try to attach a wskbd device to us. What follows
 * is definitions of callback functions and structures that are passed
 * to wscons when initializing.
 */

/*
 * Now with wscons this driver exhibits some weird behaviour.
 * It may act both as a driver of its own and the md part of the
 * wskbd driver. Therefore it can be accessed through /dev/kbd
 * and /dev/wskbd0 both.
 *
 * The data from they keyboard may end up in at least four different
 * places:
 * - If this driver has been opened (/dev/kbd) and the
 *   direct mode (TIOCDIRECT) has been set, data goes to
 *   the process who opened the device. Data will transmit itself
 *   as described by the firm_event structure.
 * - If wskbd support is compiled in and a wskbd driver has been
 *   attached then the data is sent to it. Wskbd in turn may
 *   - Send the data in the wscons_event form to a process that
 *     has opened /dev/wskbd0
 *   - Feed the data to a virtual terminal.
 * - If an ite is present the data may be fed to it.
 */

uint8_t			kbd_modifier;	/* Modifier mask		*/

static uint8_t		kbd_ring[KBD_RING_SIZE];
static volatile u_int	kbd_rbput = 0;	/* 'put' index			*/
static u_int		kbd_rbget = 0;	/* 'get' index			*/

static struct kbd_softc kbd_softc;	/* XXX */

/* {b,c}devsw[] function prototypes */
static dev_type_open(kbdopen);
static dev_type_close(kbdclose);
static dev_type_read(kbdread);
static dev_type_ioctl(kbdioctl);
static dev_type_poll(kbdpoll);
static dev_type_kqfilter(kbdkqfilter);

static void kbdsoft(void *);
static void kbdattach(device_t, device_t, void *);
static int  kbdmatch(device_t, cfdata_t, void *);
#if NITE > 0
static int  kbd_do_modifier(uint8_t);
#endif
static int  kbd_write_poll(const uint8_t *, int);
static void kbd_pkg_start(struct kbd_softc *, uint8_t);

CFATTACH_DECL_NEW(kbd, 0,
    kbdmatch, kbdattach, NULL, NULL);

const struct cdevsw kbd_cdevsw = {
	.d_open = kbdopen,
	.d_close = kbdclose,
	.d_read = kbdread,
	.d_write = nowrite,
	.d_ioctl = kbdioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = kbdpoll,
	.d_mmap = nommap,
	.d_kqfilter = kbdkqfilter,
	.d_discard = nodiscard,
	.d_flag = 0
};

#if NWSKBD>0
/* accessops */
static int	kbd_enable(void *, int);
static void	kbd_set_leds(void *, int);
static int	kbd_ioctl(void *, u_long, void *, int, struct lwp *);

/* console ops */
static void	kbd_getc(void *, u_int *, int *);
static void	kbd_pollc(void *, int);
static void	kbd_bell(void *, u_int, u_int, u_int);

static struct wskbd_accessops kbd_accessops = {
	kbd_enable,
	kbd_set_leds,
	kbd_ioctl
};

static struct wskbd_consops kbd_consops = {
	kbd_getc,
	kbd_pollc,
	kbd_bell
};

/* Pointer to keymaps. */
static struct wskbd_mapdata kbd_mapdata = {
	atarikbd_keydesctab,
	KB_US
};
#endif /* WSKBD */

/*ARGSUSED*/
static int
kbdmatch(device_t parent, cfdata_t cf, void *aux)
{

	if (!strcmp((char *)aux, "kbd"))
		return 1;
	return 0;
}

/*ARGSUSED*/
static void
kbdattach(device_t parent, device_t self, void *aux)
{
	struct kbd_softc *sc = &kbd_softc;
	int timeout;
	const uint8_t kbd_rst[]  = { 0x80, 0x01 };
	const uint8_t kbd_icmd[] = { 0x12, 0x15 };

	/*
	 * Disable keyboard interrupts from MFP
	 */
	MFP->mf_ierb &= ~IB_AINT;

	/*
	 * Reset ACIA and initialize to:
	 *    divide by 16, 8 data, 1 stop, no parity, enable RX interrupts
	 */
	KBD->ac_cs = A_RESET;
	delay(100);	/* XXX: enough? */
	KBD->ac_cs = sc->sc_soft_cs = KBD_INIT | A_RXINT;

	/*
	 * Clear error conditions
	 */
	while ((KBD->ac_cs & (A_IRQ | A_RXRDY)) != 0)
		timeout = KBD->ac_da;

	/*
	 * Now send the reset string, and read+ignore its response
	 */
	aprint_normal("\n");
	if (kbd_write_poll(kbd_rst, 2) == 0)
		aprint_error_dev(self, "error cannot reset keyboard\n");
	for (timeout = 1000; timeout > 0; timeout--) {
		if ((KBD->ac_cs & (A_IRQ | A_RXRDY)) != 0) {
			timeout = KBD->ac_da;
			timeout = 100;
		}
		delay(100);
	}
	/*
	 * Send init command: disable mice & joysticks
	 */
	kbd_write_poll(kbd_icmd, sizeof(kbd_icmd));

	sc->sc_sicookie = softint_establish(SOFTINT_SERIAL, kbdsoft, NULL);
	rnd_attach_source(&sc->sc_rndsource, device_xname(self),
	    RND_TYPE_TTY, RND_FLAG_DEFAULT);

#if NWSKBD > 0
	if (self != NULL) {
		/*
		 * Try to attach the wskbd.
		 */
		struct wskbddev_attach_args waa;

		/* Maybe should be done before this?... */
		wskbd_cnattach(&kbd_consops, NULL, &kbd_mapdata);

		waa.console = 1;
		waa.keymap = &kbd_mapdata;
		waa.accessops = &kbd_accessops;
		waa.accesscookie = NULL;
		sc->sc_wskbddev = config_found(self, &waa, wskbddevprint,
		    CFARGS_NONE);

		sc->sc_pollingmode = 0;

		kbdenable();
	}
#endif /* WSKBD */
}

void
kbdenable(void)
{
	struct kbd_softc *sc = &kbd_softc;
	int s;

	s = spltty();

	/*
	 * Clear error conditions...
	 */
	while ((KBD->ac_cs & (A_IRQ | A_RXRDY)) != 0)
		(void)KBD->ac_da;
	/*
	 * Enable interrupts from MFP
	 */
	MFP->mf_iprb  = (uint8_t)~IB_AINT;
	MFP->mf_ierb |= IB_AINT;
	MFP->mf_imrb |= IB_AINT;

	sc->sc_event_mode   = 0;
	sc->sc_events.ev_io = 0;
	sc->sc_pkg_size     = 0;
	splx(s);
}

static int
kbdopen(dev_t dev, int flags, int mode, struct lwp *l)
{
	struct kbd_softc *sc = &kbd_softc;

	if (sc->sc_events.ev_io)
		return EBUSY;

	sc->sc_events.ev_io = l->l_proc;
	ev_init(&sc->sc_events);
	return 0;
}

static int
kbdclose(dev_t dev, int flags, int mode, struct lwp *l)
{
	struct kbd_softc *sc = &kbd_softc;

	/* Turn off event mode, dump the queue */
	sc->sc_event_mode = 0;
	ev_fini(&sc->sc_events);
	sc->sc_events.ev_io = NULL;
	return 0;
}

static int
kbdread(dev_t dev, struct uio *uio, int flags)
{
	struct kbd_softc *sc = &kbd_softc;

	return ev_read(&sc->sc_events, uio, flags);
}

static int
kbdioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct kbd_softc *sc = &kbd_softc;
	struct kbdbell *kb;

	switch (cmd) {
	case KIOCTRANS:
		if (*(int *)data == TR_UNTRANS_EVENT)
			return 0;
		break;

	case KIOCGTRANS:
		/*
		 * Get translation mode
		 */
		*(int *)data = TR_UNTRANS_EVENT;
		return 0;

	case KIOCSDIRECT:
		sc->sc_event_mode = *(int *)data;
		return 0;

	case KIOCRINGBELL:
		kb = (struct kbdbell *)data;
		if (kb)
			kbd_bell_sparms(kb->volume, kb->pitch,
			    kb->duration);
		kbdbell();
		return 0;

	case FIONBIO:	/* we will remove this someday (soon???) */
		return 0;

	case FIOASYNC:
		sc->sc_events.ev_async = *(int *)data != 0;
		return 0;

	case FIOSETOWN:
		if (-*(int *)data != sc->sc_events.ev_io->p_pgid &&
		    *(int *)data != sc->sc_events.ev_io->p_pid)
			return EPERM;
		return 0;

	case TIOCSPGRP:
		if (*(int *)data != sc->sc_events.ev_io->p_pgid)
			return EPERM;
		return 0;

	default:
		return ENOTTY;
	}

	/*
	 * We identified the ioctl, but we do not handle it.
	 */
	return EOPNOTSUPP;		/* misuse, but what the heck */
}

static int
kbdpoll(dev_t dev, int events, struct lwp *l)
{
	struct kbd_softc *sc = &kbd_softc;

	return ev_poll(&sc->sc_events, events, l);
}

static int
kbdkqfilter(dev_t dev, struct knote *kn)
{
	struct kbd_softc *sc = &kbd_softc;

	return ev_kqfilter(&sc->sc_events, kn);
}

/*
 * Keyboard interrupt handler called straight from MFP at spl6.
 */
void
kbdintr(int sr)
	/* sr: sr at time of interrupt	*/
{
	struct kbd_softc *sc = &kbd_softc;
	uint8_t stat, code = 0 /* XXX gcc */;
	uint32_t rndstat;
	bool got_char = false;

	/*
	 * There may be multiple keys available. Read them all.
	 */
	stat = KBD->ac_cs;
	rndstat = stat;
	while ((stat & (A_RXRDY | A_OE | A_PE)) != 0) {
		got_char = true;
		if ((KBD->ac_cs & (A_OE | A_PE)) == 0) {
			code = KBD->ac_da;
			kbd_ring[kbd_rbput++ & KBD_RING_MASK] = code;
		} else {
			/* Silently ignore errors */
			code = KBD->ac_da;
		}
		stat = KBD->ac_cs;
	}

	/*
	 * If characters are waiting for transmit, send them.
	 */
	if ((sc->sc_soft_cs & A_TXINT) != 0 &&
	    (KBD->ac_cs & A_TXRDY) != 0) {
		if (sc->sc_sendp != NULL)
			KBD->ac_da = *sc->sc_sendp++;
		if (--sc->sc_send_cnt <= 0) {
			/*
			 * The total package has been transmitted,
			 * wakeup anyone waiting for it.
			 */
			KBD->ac_cs = (sc->sc_soft_cs &= ~A_TXINT);
			sc->sc_sendp = NULL;
			sc->sc_send_cnt = 0;
			wakeup((void *)&sc->sc_send_cnt);
		}
	}

	/*
	 * Activate software-level to handle possible input.
	 * Also add status and data to the rnd(9) pool.
	 */
	if (got_char) {
		softint_schedule(sc->sc_sicookie);
		rnd_add_uint32(&sc->sc_rndsource, (rndstat << 8) | code);
	}
}

/*
 * Keyboard soft interrupt handler
 */
static void
kbdsoft(void *junk1)
{
	struct kbd_softc *sc = &kbd_softc;
	int s;
	uint8_t code;
	struct firm_event *fe;
	int put, get, n;

	get = kbd_rbget;

	for (;;) {
		n = kbd_rbput;
		if (get == n) /* We're done	*/
			break;
		n -= get;
		if (n > KBD_RING_SIZE) { /* Ring buffer overflow	*/
			get += n - KBD_RING_SIZE;
			n    = KBD_RING_SIZE;
		}
		while (--n >= 0) {
			code = kbd_ring[get++ & KBD_RING_MASK];

			/*
			 * If collecting a package, stuff it in and
			 * continue.
			 */
			if (sc->sc_pkg_size &&
			    (sc->sc_pkg_idx < sc->sc_pkg_size)) {
				sc->sc_package[sc->sc_pkg_idx++] = code;
				if (sc->sc_pkg_idx == sc->sc_pkg_size) {
					/*
					 * Package is complete.
					 */
					switch (sc->sc_pkg_type) {
#if NMOUSE > 0
					case KBD_AMS_PKG:
					case KBD_RMS_PKG:
					case KBD_JOY1_PKG:
						mouse_soft(
						    (REL_MOUSE *)sc->sc_package,
						    sc->sc_pkg_size,
						    sc->sc_pkg_type);
#endif /* NMOUSE */
					}
					sc->sc_pkg_size = 0;
				}
				continue;
			}
			/*
			 * If this is a package header, init pkg. handling.
			 */
			if (!KBD_IS_KEY(code)) {
				kbd_pkg_start(sc, code);
				continue;
			}
#if NWSKBD > 0
			/*
			 * If we have attached a wskbd and not in polling mode
			 * and nobody has opened us directly, then send the
			 * keystroke to the wskbd.
			 */

			if (sc->sc_pollingmode == 0 &&
			    sc->sc_wskbddev != NULL &&
			    sc->sc_event_mode == 0) {
				wskbd_input(sc->sc_wskbddev,
				    KBD_RELEASED(code) ?
				    WSCONS_EVENT_KEY_UP :
				    WSCONS_EVENT_KEY_DOWN,
				    KBD_SCANCODE(code));
				continue;
			}
#endif /* NWSKBD */
#if NITE > 0
			if (kbd_do_modifier(code) && !sc->sc_event_mode)
				continue;
#endif

			/*
			 * if not in event mode, deliver straight to ite to
			 * process key stroke
			 */
			if (!sc->sc_event_mode) {
				/* Gets to spltty() by itself	*/
#if NITE > 0
				ite_filter(code, ITEFILT_TTY);
#endif
				continue;
			}

			/*
			 * Keyboard is generating events.  Turn this keystroke
			 * into an event and put it in the queue.  If the queue
			 * is full, the keystroke is lost (sorry!).
			 */
			s = spltty();
			put = sc->sc_events.ev_put;
			fe  = &sc->sc_events.ev_q[put];
			put = (put + 1) % EV_QSIZE;
			if (put == sc->sc_events.ev_get) {
				log(LOG_WARNING,
				    "keyboard event queue overflow\n");
				splx(s);
				continue;
			}
			fe->id    = KBD_SCANCODE(code);
			fe->value = KBD_RELEASED(code) ? VKEY_UP : VKEY_DOWN;
			firm_gettime(fe);
			sc->sc_events.ev_put = put;
			EV_WAKEUP(&sc->sc_events);
			splx(s);
		}
		kbd_rbget = get;
	}
}

static uint8_t sound[] = {
	0xA8, 0x01, 0xA9, 0x01, 0xAA, 0x01, 0x00,
	0xF8, 0x10, 0x10, 0x10, 0x00, 0x20, 0x03
};

void
kbdbell(void)
{
	int i, s;

	s = splhigh();
	for (i = 0; i < sizeof(sound); i++) {
		YM2149->sd_selr = i;
		YM2149->sd_wdat = sound[i];
	}
	splx(s);
}


/*
 * Set the parameters of the 'default' beep.
 */

#define KBDBELLCLOCK	125000	/* 2MHz / 16 */
#define KBDBELLDURATION	128	/* 256 / 2MHz */

void
kbd_bell_gparms(u_int *volume, u_int *pitch, u_int *duration)
{
	u_int tmp;

	tmp = sound[11] | (sound[12] << 8);
	*duration = (tmp * KBDBELLDURATION) / 1000;

	tmp = sound[0] | (sound[1] << 8);
	*pitch = KBDBELLCLOCK / tmp;

	*volume = 0;
}

void
kbd_bell_sparms(u_int volume, u_int pitch, u_int duration)
{
	u_int f, t;

	f = pitch > 10 ? pitch : 10;	/* minimum pitch */
	if (f > 20000)
		f = 20000;		/* maximum pitch */

	f = KBDBELLCLOCK / f;

	t = (duration * 1000) / KBDBELLDURATION;

	sound[ 0] = f & 0xff;
	sound[ 1] = (f >> 8) & 0xf;
	f -= 1;
	sound[ 2] = f & 0xff;
	sound[ 3] = (f >> 8) & 0xf;
	f += 2;
	sound[ 4] = f & 0xff;
	sound[ 5] = (f >> 8) & 0xf;

	sound[11] = t & 0xff;
	sound[12] = (t >> 8) & 0xff;

	sound[13] = 0x03;
}

int
kbdgetcn(void)
{
	uint8_t code;
	int s = spltty();
	int ints_active;

	ints_active = 0;
	if ((MFP->mf_imrb & IB_AINT) != 0) {
		ints_active   = 1;
		MFP->mf_imrb &= ~IB_AINT;
	}
	for (;;) {
		while (!((KBD->ac_cs & (A_IRQ | A_RXRDY)) == (A_IRQ | A_RXRDY)))
			continue;	/* Wait for key	*/
		if ((KBD->ac_cs & (A_OE | A_PE)) != 0) {
			code = KBD->ac_da;	/* Silently ignore errors */
			continue;
		}
		code = KBD->ac_da;
#if NITE>0
		if (!kbd_do_modifier(code))
#endif
			break;
	}

	if (ints_active) {
		MFP->mf_iprb  = (uint8_t)~IB_AINT;
		MFP->mf_imrb |=  IB_AINT;
	}

	splx(s);
	return code;
}

/*
 * Write a command to the keyboard in 'polled' mode.
 */
static int
kbd_write_poll(const uint8_t *cmd, int len)
{
	int timeout;

	while (len-- > 0) {
		KBD->ac_da = *cmd++;
		for (timeout = 100; (KBD->ac_cs & A_TXRDY) == 0; timeout--)
			delay(10);
		if ((KBD->ac_cs & A_TXRDY) == 0)
			return 0;
	}
	return 1;
}

/*
 * Write a command to the keyboard. Return when command is send.
 */
void
kbd_write(const uint8_t *cmd, int len)
{
	struct kbd_softc *sc = &kbd_softc;
	int s;

	/*
	 * Get to splhigh, 'real' interrupts arrive at spl6!
	 */
	s = splhigh();

	/*
	 * Make sure any previous write has ended...
	 */
	while (sc->sc_sendp != NULL)
		tsleep((void *)&sc->sc_sendp, TTOPRI, "kbd_write1", 0);

	/*
	 * If the KBD-acia is not currently busy, send the first
	 * character now.
	 */
	KBD->ac_cs = (sc->sc_soft_cs |= A_TXINT);
	if ((KBD->ac_cs & A_TXRDY) != 0) {
		KBD->ac_da = *cmd++;
		len--;
	}

	/*
	 * If we're not yet done, wait until all characters are send.
	 */
	if (len > 0) {
		sc->sc_sendp    = cmd;
		sc->sc_send_cnt = len;
		tsleep((void *)&sc->sc_send_cnt, TTOPRI, "kbd_write2", 0);
	}
	splx(s);

	/*
	 * Wakeup all procs waiting for us.
	 */
	wakeup((void *)&sc->sc_sendp);
}

/*
 * Setup softc-fields to assemble a keyboard package.
 */
static void
kbd_pkg_start(struct kbd_softc *sc, uint8_t msg_start)
{

	sc->sc_pkg_idx    = 1;
	sc->sc_package[0] = msg_start;
	switch (msg_start) {
	case 0xf6:
		sc->sc_pkg_type = KBD_MEM_PKG;
		sc->sc_pkg_size = 8;
		break;
	case 0xf7:
		sc->sc_pkg_type = KBD_AMS_PKG;
		sc->sc_pkg_size = 6;
		break;
	case 0xf8:
	case 0xf9:
	case 0xfa:
	case 0xfb:
		sc->sc_pkg_type = KBD_RMS_PKG;
		sc->sc_pkg_size = 3;
		break;
	case 0xfc:
		sc->sc_pkg_type = KBD_CLK_PKG;
		sc->sc_pkg_size = 7;
		break;
	case 0xfe:
		sc->sc_pkg_type = KBD_JOY0_PKG;
		sc->sc_pkg_size = 2;
		break;
	case 0xff:
		sc->sc_pkg_type = KBD_JOY1_PKG;
		sc->sc_pkg_size = 2;
		break;
	default:
		printf("kbd: Unknown packet 0x%x\n", msg_start);
		break;
	}
}

#if NITE > 0
/*
 * Modifier processing
 */
static int
kbd_do_modifier(uint8_t code)
{
	uint8_t up, mask;

	up   = KBD_RELEASED(code);
	mask = 0;

	switch (KBD_SCANCODE(code)) {
	case KBD_LEFT_SHIFT:
		mask = KBD_MOD_LSHIFT;
		break;
	case KBD_RIGHT_SHIFT:
		mask = KBD_MOD_RSHIFT;
		break;
	case KBD_CTRL:
		mask = KBD_MOD_CTRL;
		break;
	case KBD_ALT:
		mask = KBD_MOD_ALT;
		break;
	case KBD_CAPS_LOCK:
		/* CAPSLOCK is a toggle */
		if (!up)
			kbd_modifier ^= KBD_MOD_CAPS;
		return 1;
	}
	if (mask) {
		if (up)
			kbd_modifier &= ~mask;
		else
			kbd_modifier |= mask;
		return 1;
	}
	return 0;
}
#endif

#if NWSKBD > 0
/*
 * These are the callback functions that are passed to wscons.
 * They really don't do anything worth noting, just call the
 * other functions above.
 */

static int
kbd_enable(void *c, int on)
{

	/* Wonder what this is supposed to do... */
	return 0;
}

static void
kbd_set_leds(void *c, int leds)
{

	/* we can not set the leds */
}

static int
kbd_ioctl(void *c, u_long cmd, void *data, int flag, struct lwp *p)
{
	struct wskbd_bell_data *kd;

	switch (cmd) {
	case WSKBDIO_COMPLEXBELL:
		kd = (struct wskbd_bell_data *)data;
		kbd_bell(0, kd->pitch, kd->period, kd->volume);
		return 0;
	case WSKBDIO_SETLEDS:
		return 0;
	case WSKBDIO_GETLEDS:
		*(int *)data = 0;
		return 0;
	case WSKBDIO_GTYPE:
		*(u_int *)data = WSKBD_TYPE_ATARI;
		return 0;
	}

	/*
	 * We are supposed to return EPASSTHROUGH to wscons if we didn't
	 * understand.
	 */
	return EPASSTHROUGH;
}

static void
kbd_getc(void *c, u_int *type, int *data)
{
	int key;

	key = kbdgetcn();

	*data = KBD_SCANCODE(key);
	*type = KBD_RELEASED(key) ? WSCONS_EVENT_KEY_UP : WSCONS_EVENT_KEY_DOWN;
}

static void
kbd_pollc(void *c, int on)
{
	struct kbd_softc *sc = &kbd_softc;

	sc->sc_pollingmode = on;
}

static void
kbd_bell(void *v, u_int pitch, u_int duration, u_int volume)
{

	kbd_bell_sparms(volume, pitch, duration);
	kbdbell();
}
#endif /* NWSKBD */
