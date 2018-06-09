// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    Videa Gridlee hardware

    driver by Aaron Giles

    special thanks to Howard Delman, Roger Hector and Ed Rotberg for
    allowing distribution of the Gridlee ROMs

    Based on the Bally/Sente SAC system

    Games supported:
        * Gridlee

    Known bugs:
        * analog sound hardware is unemulated

****************************************************************************

    Memory map

****************************************************************************

    ========================================================================
    CPU #1
    ========================================================================
    0000-007F   R/W   xxxxxxxx    Sprite RAM (32 entries x 4 bytes)
                R/W   xxxxxxxx       (0: image number)
                R/W   --------       (1: unused?)
                R/W   xxxxxxxx       (2: Y position, offset by 17 pixels)
                R/W   xxxxxxxx       (3: X position)
    0080-07FF   R/W   xxxxxxxx    Program RAM
    0800-7FFF   R/W   xxxxxxxx    Video RAM (256x240 pixels)
                R/W   xxxx----       (left pixel)
                R/W   ----xxxx       (right pixel)
    9000          W   -------x    Player 1 LED
    9010          W   -------x    Player 2 LED
    9020          W   -------x    Coin counter
    9060          W   -------x    Unknown (written once at startup)
    9070          W   -------x    Cocktail flip
    9200          W   --xxxxxx    Palette base select
    9380          W   --------    Watchdog reset
    9500        R     ---xxxxx    Trackball Y position
                R     ---x----    Sign of delta
                R     ----xxxx    Cumulative magnitude
    9501        R     ---xxxxx    Trackball X position
                R     ---x----    Sign of delta
                R     ----xxxx    Cumulative magnitude
    9502        R     ------x-    Fire button 2
                R     -------x    Fire button 1
    9503        R     --xx----    Coinage switches
                R     ----x---    2 player start
                R     -----x--    1 player start
                R     ------x-    Right coin
                R     -------x    Left coin
    9600        R     x-------    Reset game data switch
                R     -x------    Reset hall of fame switch
                R     --x-----    Cocktail/upright switch
                R     ---x----    Free play switch
                R     ----xx--    Lives switches
                R     ------xx    Bonus lives switches
    9700        R     x-------    VBLANK
                R     -x------    Service advance
                R     --x-----    Service switch
    9820        R     xxxxxxxx    Random number generator
    9828-982C     W   ????????    Unknown
    9830-983F     W   ????????    Unknown (sound-related)
    9C00-9CFF   R/W   --------    NVRAM
    A000-FFFF   R     xxxxxxxx    Fixed program ROM
    ========================================================================
    Interrupts:
        NMI not connected
        IRQ generated by 32L
        FIRQ generated by ??? (but should be around scanline 92)
    ========================================================================

***************************************************************************/


#include "emu.h"
#include "includes/gridlee.h"

#include "cpu/m6809/m6809.h"
#include "sound/samples.h"
#include "machine/nvram.h"
#include "machine/watchdog.h"
#include "speaker.h"


/* constants */
#define FIRQ_SCANLINE 92

/*************************************
 *
 *  Interrupt handling
 *
 *************************************/

TIMER_CALLBACK_MEMBER(gridlee_state::irq_off_tick)
{
	m_maincpu->set_input_line(M6809_IRQ_LINE, CLEAR_LINE);
}


TIMER_CALLBACK_MEMBER(gridlee_state::irq_timer_tick)
{
	/* next interrupt after scanline 256 is scanline 64 */
	if (param == 256)
		m_irq_timer->adjust(m_screen->time_until_pos(64), 64);
	else
		m_irq_timer->adjust(m_screen->time_until_pos(param + 64), param + 64);

	/* IRQ starts on scanline 0, 64, 128, etc. */
	m_maincpu->set_input_line(M6809_IRQ_LINE, ASSERT_LINE);

	/* it will turn off on the next HBLANK */
	m_irq_off->adjust(m_screen->time_until_pos(param, GRIDLEE_HBSTART));
}


TIMER_CALLBACK_MEMBER(gridlee_state::firq_off_tick)
{
	m_maincpu->set_input_line(M6809_FIRQ_LINE, CLEAR_LINE);
}


TIMER_CALLBACK_MEMBER(gridlee_state::firq_timer_tick)
{
	/* same time next frame */
	m_firq_timer->adjust(m_screen->time_until_pos(FIRQ_SCANLINE));

	/* IRQ starts on scanline FIRQ_SCANLINE? */
	m_maincpu->set_input_line(M6809_FIRQ_LINE, ASSERT_LINE);

	/* it will turn off on the next HBLANK */
	m_firq_off->adjust(m_screen->time_until_pos(FIRQ_SCANLINE, GRIDLEE_HBSTART));
}

void gridlee_state::machine_start()
{
	/* create the polynomial tables */
	poly17_init();

	save_item(NAME(m_last_analog_input));
	save_item(NAME(m_last_analog_output));

	m_irq_off = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(gridlee_state::irq_off_tick),this));
	m_irq_timer = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(gridlee_state::irq_timer_tick),this));
	m_firq_off = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(gridlee_state::firq_off_tick),this));
	m_firq_timer = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(gridlee_state::firq_timer_tick),this));
}


void gridlee_state::machine_reset()
{
	/* start timers to generate interrupts */
	m_irq_timer->adjust(m_screen->time_until_pos(0));
	m_firq_timer->adjust(m_screen->time_until_pos(FIRQ_SCANLINE));
}



/*************************************
 *
 *  ADC handlers
 *
 *************************************/

READ8_MEMBER(gridlee_state::analog_port_r)
{
	int delta, sign, magnitude;
	uint8_t newval;
	static const char *const portnames[] = { "TRACK0_Y", "TRACK0_X", "TRACK1_Y", "TRACK1_X" };

	/* first read the new trackball value and compute the signed delta */
	newval = ioport(portnames[offset + 2 * m_cocktail_flip])->read();
	delta = (int)newval - (int)m_last_analog_input[offset];

	/* handle the case where we wrap around from 0x00 to 0xff, or vice versa */
	if (delta >= 0x80)
		delta -= 0x100;
	if (delta <= -0x80)
		delta += 0x100;

	/* just return the previous value for deltas less than 2, which are ignored */
	if (delta >= -1 && delta <= 1)
		return m_last_analog_output[offset];
	m_last_analog_input[offset] = newval;

	/* compute the sign and the magnitude */
	sign = (delta < 0) ? 0x10 : 0x00;
	magnitude = (delta < 0) ? -delta : delta;

	/* add the magnitude to the running total */
	m_last_analog_output[offset] += magnitude;

	/* or in the sign bit and return that */
	return (m_last_analog_output[offset] & 15) | sign;
}



/*************************************
 *
 *  MM5837 noise generator
 *
 *  NOTE: this is stolen straight from
 *          POKEY.c
 *
 *  NOTE: this is assumed to be the
 *          same as balsente.c
 *
 *************************************/

#define POLY17_BITS 17
#define POLY17_SIZE ((1 << POLY17_BITS) - 1)
#define POLY17_SHL  7
#define POLY17_SHR  10
#define POLY17_ADD  0x18000

void gridlee_state::poly17_init()
{
	uint32_t i, x = 0;
	uint8_t *p, *r;

	/* allocate memory */
	m_poly17 = std::make_unique<uint8_t[]>(2 * (POLY17_SIZE + 1));
	p = m_poly17.get();
	r = m_rand17 = m_poly17.get() + POLY17_SIZE + 1;

	/* generate the polynomial */
	for (i = 0; i < POLY17_SIZE; i++)
	{
		/* store new values */
		*p++ = x & 1;
		*r++ = x >> 3;

		/* calculate next bit */
		x = ((x << POLY17_SHL) + (x >> POLY17_SHR) + POLY17_ADD) & POLY17_SIZE;
	}
}



/*************************************
 *
 *  Hardware random numbers
 *
 *************************************/

READ8_MEMBER(gridlee_state::random_num_r)
{
	uint32_t cc;

	/* CPU runs at 1.25MHz, noise source at 100kHz --> multiply by 12.5 */
	cc = m_maincpu->total_cycles();

	/* 12.5 = 8 + 4 + 0.5 */
	cc = (cc << 3) + (cc << 2) + (cc >> 1);
	return m_rand17[cc & POLY17_SIZE];
}



/*************************************
 *
 *  Misc handlers
 *
 *************************************/

WRITE_LINE_MEMBER(gridlee_state::coin_counter_w)
{
	machine().bookkeeping().coin_counter_w(0, state);
	logerror("coin counter %s\n", state ? "on" : "off");
}



/*************************************
 *
 *  Main CPU memory handlers
 *
 *************************************/

/* CPU 1 read addresses */
void gridlee_state::cpu1_map(address_map &map)
{
	map(0x0000, 0x07ff).ram().share("spriteram");
	map(0x0800, 0x7fff).ram().w(FUNC(gridlee_state::gridlee_videoram_w)).share("videoram");
	map(0x9000, 0x9000).select(0x0070).lw8("latch_w",
										   [this](address_space &space, offs_t offset, u8 data, u8 mem_mask) {
											   m_latch->write_d0(space, offset >> 4, data, mem_mask);
										   });
	map(0x9200, 0x9200).w(FUNC(gridlee_state::gridlee_palette_select_w));
	map(0x9380, 0x9380).w("watchdog", FUNC(watchdog_timer_device::reset_w));
	map(0x9500, 0x9501).r(FUNC(gridlee_state::analog_port_r));
	map(0x9502, 0x9502).portr("IN0");
	map(0x9503, 0x9503).portr("IN1");
	map(0x9600, 0x9600).portr("DSW");
	map(0x9700, 0x9700).portr("IN2").nopw();
	map(0x9820, 0x9820).r(FUNC(gridlee_state::random_num_r));
	map(0x9828, 0x993f).w("gridlee", FUNC(gridlee_sound_device::gridlee_sound_w));
	map(0x9c00, 0x9cff).ram().share("nvram");
	map(0xa000, 0xffff).rom();
}



/*************************************
 *
 *  Port definitions
 *
 *************************************/

static INPUT_PORTS_START( gridlee )
	PORT_START("TRACK0_Y")  /* 9500 (fake) */
	PORT_BIT( 0xff, 0, IPT_TRACKBALL_Y ) PORT_SENSITIVITY(20) PORT_KEYDELTA(8)

	PORT_START("TRACK0_X")  /* 9501 (fake) */
	PORT_BIT( 0xff, 0, IPT_TRACKBALL_X ) PORT_SENSITIVITY(20) PORT_KEYDELTA(8) PORT_REVERSE

	PORT_START("TRACK1_Y")  /* 9500 (fake) */
	PORT_BIT( 0xff, 0, IPT_TRACKBALL_Y ) PORT_SENSITIVITY(20) PORT_KEYDELTA(8) PORT_COCKTAIL

	PORT_START("TRACK1_X")  /* 9501 (fake) */
	PORT_BIT( 0xff, 0, IPT_TRACKBALL_X ) PORT_SENSITIVITY(20) PORT_KEYDELTA(8) PORT_REVERSE PORT_COCKTAIL

	PORT_START("IN0")       /* 9502 */
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 )
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_COCKTAIL
	PORT_BIT( 0xfc, IP_ACTIVE_LOW, IPT_UNKNOWN )

	PORT_START("IN1")       /* 9503 */
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_COIN1 )
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_COIN2 )
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_START1 )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_START2 )
	PORT_DIPNAME( 0x30, 0x00, DEF_STR( Coinage ))
	PORT_DIPSETTING(    0x20, DEF_STR( 2C_1C ))
	PORT_DIPSETTING(    0x00, DEF_STR( 1C_1C ))
	PORT_DIPSETTING(    0x10, DEF_STR( 1C_2C ))
	PORT_BIT( 0xc0, IP_ACTIVE_LOW, IPT_UNKNOWN )

	PORT_START("DSW")       /* 9600 */
	PORT_DIPNAME( 0x03, 0x01, DEF_STR( Bonus_Life ))
	PORT_DIPSETTING(    0x00, "8000 points" )
	PORT_DIPSETTING(    0x01, "10000 points" )
	PORT_DIPSETTING(    0x02, "12000 points" )
	PORT_DIPSETTING(    0x03, "14000 points" )
	PORT_DIPNAME( 0x0c, 0x04, DEF_STR( Lives ))
	PORT_DIPSETTING(    0x00, "2" )
	PORT_DIPSETTING(    0x04, "3" )
	PORT_DIPSETTING(    0x08, "4" )
	PORT_DIPSETTING(    0x0c, "5" )
	PORT_DIPNAME( 0x10, 0x00, DEF_STR( Free_Play ))
	PORT_DIPSETTING(    0x00, DEF_STR( Off ))
	PORT_DIPSETTING(    0x10, DEF_STR( On ))
	PORT_DIPNAME( 0x20, 0x00, DEF_STR( Cabinet ) )
	PORT_DIPSETTING(    0x00, DEF_STR( Upright ) )
	PORT_DIPSETTING(    0x20, DEF_STR( Cocktail ) )
	PORT_DIPNAME( 0x40, 0x00, "Reset Hall of Fame" )
	PORT_DIPSETTING(    0x00, DEF_STR( No ))
	PORT_DIPSETTING(    0x40, DEF_STR( Yes ))
	PORT_DIPNAME( 0x80, 0x00, "Reset Game Data" )
	PORT_DIPSETTING(    0x00, DEF_STR( No ))
	PORT_DIPSETTING(    0x80, DEF_STR( Yes ))

	PORT_START("IN2")       /* 9700 */
	PORT_BIT( 0x1f, IP_ACTIVE_LOW, IPT_UNKNOWN )
	PORT_SERVICE( 0x20, IP_ACTIVE_LOW )
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_SERVICE1 )
	PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_CUSTOM ) PORT_VBLANK("screen")
INPUT_PORTS_END



/*************************************
 *
 *  Sound definitions
 *
 *************************************/

static const char *const sample_names[] =
{
	"*gridlee",
	"bounce1",
	"bounce2",
	nullptr   /* end of array */
};

/*************************************
 *
 *  Machine driver
 *
 *************************************/

MACHINE_CONFIG_START(gridlee_state::gridlee)

	/* basic machine hardware */
	MCFG_DEVICE_ADD("maincpu", M6809, GRIDLEE_CPU_CLOCK)
	MCFG_DEVICE_PROGRAM_MAP(cpu1_map)

	MCFG_NVRAM_ADD_0FILL("nvram")

	MCFG_WATCHDOG_ADD("watchdog")

	MCFG_DEVICE_ADD("latch", LS259, 0) // type can only be guessed
	MCFG_ADDRESSABLE_LATCH_Q0_OUT_CB(OUTPUT("led0"))
	MCFG_ADDRESSABLE_LATCH_Q1_OUT_CB(OUTPUT("led1"))
	MCFG_ADDRESSABLE_LATCH_Q2_OUT_CB(WRITELINE(*this, gridlee_state, coin_counter_w))
	// Q6 unknown - only written to at startup
	MCFG_ADDRESSABLE_LATCH_Q7_OUT_CB(WRITELINE(*this, gridlee_state, cocktail_flip_w))

	/* video hardware */
	MCFG_SCREEN_ADD("screen", RASTER)
	MCFG_SCREEN_RAW_PARAMS(GRIDLEE_PIXEL_CLOCK, GRIDLEE_HTOTAL, GRIDLEE_HBEND, GRIDLEE_HBSTART, GRIDLEE_VTOTAL, GRIDLEE_VBEND, GRIDLEE_VBSTART)
	MCFG_SCREEN_UPDATE_DRIVER(gridlee_state, screen_update_gridlee)
	MCFG_SCREEN_PALETTE("palette")

	MCFG_PALETTE_ADD("palette", 2048)
	MCFG_PALETTE_INIT_OWNER(gridlee_state,gridlee)

	/* sound hardware */
	SPEAKER(config, "mono").front_center();

	MCFG_DEVICE_ADD("gridlee", GRIDLEE, 0)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 1.0)

	MCFG_DEVICE_ADD("samples", SAMPLES)
	MCFG_SAMPLES_CHANNELS(8)
	MCFG_SAMPLES_NAMES(sample_names)
	MCFG_SOUND_ROUTE(ALL_OUTPUTS, "mono", 0.40)
MACHINE_CONFIG_END



/*************************************
 *
 *  ROM definitions
 *
 *************************************/

ROM_START( gridlee )
	ROM_REGION( 0x10000, "maincpu", 0 )
	ROM_LOAD( "gridfnla.bin", 0xa000, 0x1000, CRC(1c43539e) SHA1(8b4a6f5c2c22bb021937157606d2129e2b01f718) )
	ROM_LOAD( "gridfnlb.bin", 0xb000, 0x1000, CRC(c48b91b8) SHA1(651210470ddf7c14f16f6c3046a9b8e903824ab8) )
	ROM_LOAD( "gridfnlc.bin", 0xc000, 0x1000, CRC(6ad436dd) SHA1(f393b63077f249d34a8e85649aea58b27a0425b1) )
	ROM_LOAD( "gridfnld.bin", 0xd000, 0x1000, CRC(f7188ddb) SHA1(eeb3f7dd8c61689cdd9992280ee1b3b5dc79a54c) )
	ROM_LOAD( "gridfnle.bin", 0xe000, 0x1000, CRC(d5330bee) SHA1(802bb5705d4cd22d556c1bcbcf730d688ca8e8ab) )
	ROM_LOAD( "gridfnlf.bin", 0xf000, 0x1000, CRC(695d16a3) SHA1(53d22cbedbedad8c89a964b6a38b7075c43cf03b) )

	ROM_REGION( 0x4000, "gfx1", 0 )
	ROM_LOAD( "gridpix0.bin", 0x0000, 0x1000, CRC(e6ea15ae) SHA1(2c482e25ea44aafd63ca5533b5a2e2dd8bf89365) )
	ROM_LOAD( "gridpix1.bin", 0x1000, 0x1000, CRC(d722f459) SHA1(8cad028eefbba387bdd57fb8bb3a855ae314fb32) )
	ROM_LOAD( "gridpix2.bin", 0x2000, 0x1000, CRC(1e99143c) SHA1(89c2f772cd15f2c37c8167a03dc4c7d1c923e4c3) )
	ROM_LOAD( "gridpix3.bin", 0x3000, 0x1000, CRC(274342a0) SHA1(818cfd4132183d922ff4585c73f2cd6e4546c75b) )

	ROM_REGION( 0x1800, "proms", 0 )
	ROM_LOAD( "grdrprom.bin", 0x0000, 0x800, CRC(f28f87ed) SHA1(736f38c3ec5455de1266aad348ba708d7201b21a) )
	ROM_LOAD( "grdgprom.bin", 0x0800, 0x800, CRC(921b0328) SHA1(59d1a3d3a90bd680a75adca5dd1b4682236c673b) )
	ROM_LOAD( "grdbprom.bin", 0x1000, 0x800, CRC(04350348) SHA1(098fec3073143e0b8746e728d7d321f2a353286f) )
ROM_END



/*************************************
 *
 *  Game drivers
 *
 *************************************/

GAME( 1983, gridlee, 0, gridlee, gridlee, gridlee_state, empty_init, ROT0, "Videa", "Gridlee", MACHINE_SUPPORTS_SAVE | MACHINE_IMPERFECT_SOUND )
