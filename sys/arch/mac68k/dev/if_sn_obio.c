/*	$OpenBSD: if_sn_obio.c,v 1.5 1997/03/29 23:26:49 briggs Exp $	*/

/*
 * Copyright (C) 1997 Allen Briggs
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Allen Briggs
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/systm.h>

#include <net/if.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <machine/bus.h>
#include <machine/macinfo.h>
#include <machine/viareg.h>

#include "obiovar.h"
#include "if_snreg.h"
#include "if_snvar.h"

#define SONIC_REG_BASE	0x50F0A000
#define SONIC_PROM_BASE	0x50F08000

static int	sn_obio_match __P((struct device *, void *, void *));
static void	sn_obio_attach __P((struct device *, struct device *, void *));
static void	sn_obio_getaddr __P((struct sn_softc *));

struct cfattach sn_obio_ca = {
	sizeof(struct sn_softc), sn_obio_match, sn_obio_attach
};

static int
sn_obio_match(parent, vcf, aux)
	struct device *parent;
	void *vcf;
	void *aux;
{
	if (mac68k_machine.sonic)
		return 1;

	return 0;
}

/*
 * Install interface into kernel networking data structures
 */
static void
sn_obio_attach(parent, self, aux)
	struct device *parent, *self;
	void   *aux;
{
	struct obio_attach_args *oa = (struct obio_attach_args *) aux;
        struct sn_softc *sc = (void *)self;
	int i;

	sc->snr_dcr = DCR_SYNC | DCR_WAIT0 | DCR_DMABLOCK |
			DCR_RFT16 | DCR_TFT16;
	sc->snr_dcr2 = 0;

	switch (current_mac_model->machineid) {
	case MACH_MACQ700:	/* only tested on Q700 */
	case MACH_MACQ900:
	case MACH_MACQ950:
		sc->snr_dcr |= DCR_LBR | DCR_DW32;
		sc->bitmode = 1;
		break;
  
	case MACH_MACQ800:
	case MACH_MACQ650:
	case MACH_MACC650:
	case MACH_MACC610:
	case MACH_MACQ610:
		sc->snr_dcr |= DCR_EXBUS | DCR_DW32;
		sc->bitmode = 1;
		break;

	case MACH_MACPB500:
		sc->snr_dcr |= DCR_LBR | DCR_DW16;
  		sc->bitmode = 0;
		break;
        }

	sc->sc_regt = oa->oa_tag;
	if (bus_space_map(sc->sc_regt, SONIC_REG_BASE, SN_REGSIZE,
				0, &sc->sc_regh)) {
		panic("failed to map space for SONIC regs.\n");
	}

	sc->slotno = 9;

	sn_obio_getaddr(sc);

	/* regs are addressed as words, big-endian. */
	for (i = 0; i < SN_NREGS; i++) {
		sc->sc_reg_map[i] = (bus_size_t)((i * 4) + 2);
	}

	/* snsetup returns 1 if something fails */
	if (snsetup(sc)) {
		bus_space_unmap(sc->sc_regt, sc->sc_regh, SN_REGSIZE);
		return;
	}

	add_nubus_intr(sc->slotno, snintr, (void *)sc);
}

static u_char bbr4[] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
#define bbr(v)	((bbr4[(v)&0xf] << 4) | bbr4[((v)>>4) & 0xf])

static void
sn_obio_getaddr(sc)
	struct sn_softc	*sc;
{
	bus_space_handle_t	bsh;
	int			i, do_bbr;
	u_char			b;

	if (bus_space_map(sc->sc_regt, SONIC_PROM_BASE, NBPG, 0, &bsh)) {
		panic("failed to map space to read SONIC address.\n");
	}

	/*
	 * For reasons known only to Apple, MAC addresses in the ethernet
	 * PROM are stored in Token Ring (IEEE 802.5) format, that is
	 * with all of the bits in each byte reversed (canonical bit format).
	 * When the address is read out it must be reversed to ethernet format
	 * before use.
	 *
	 * Apple has been assigned OUI's 00:08:07 and 00:a0:40. All onboard
	 * ethernet addresses on 68K machines should be in one of these
	 * two ranges.
	 *
	 * Here is where it gets complicated.
	 *
	 * The PMac 7200, 7500, 8500, and 9500 accidentally had the PROM
	 * written in standard ethernet format. The MacOS accounted for this
	 * in these systems, and did not reverse the bytes. Some other
	 * networking utilities were not so forgiving, and got confused.
	 * "Some" of Apple's Nubus ethernet cards also had their bits
	 * burned in ethernet format.
	 *
	 * Apple petitioned the IEEE and was granted the 00:05:02 (bit reversal
	 * of 00:a0:40) as well. As of OpenTransport 1.1.1, Apple removed
	 * their workaround and now reverses the bits regardless of
	 * what kind of machine it is. So PMac systems and the affected
	 * Nubus cards now use 00:05:02, instead of the 00:a0:40 for which they
	 * were intended.
	 *
	 * See Apple Techinfo article TECHINFO-0020552, "OpenTransport 1.1.1
	 * and MacOS System 7.5.3 FAQ (10/96)" for more details.
	 */
	do_bbr = 0;
	b = bus_space_read_1(sc->sc_regt, bsh, 0);
	if (b == 0x10)
		do_bbr = 1;
	sc->sc_arpcom.ac_enaddr[0] = (do_bbr) ? bbr(b) : b;

	for (i = 1 ; i < ETHER_ADDR_LEN ; i++) {
		b = bus_space_read_1(sc->sc_regt, bsh, i);
		sc->sc_arpcom.ac_enaddr[i] = (do_bbr) ? bbr(b) : b;
	}

	bus_space_unmap(sc->sc_regt, bsh, NBPG);
}
