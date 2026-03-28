#include "common.h"
#include <string.h>
#include <stdio.h>
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "pico/binary_info.h"
#include "ntrCard.pio.h"
#include "romData.h"
#include "pico/multicore.h"
#include "blowfish.h"
#include "scrambler.h"
#include "sd/fatfs/ff.h"
#include "ntrCardRom.h"
#include "ntrCardSpiUart.h"
#include "r4.h"
#include "sd/SdCard.h"
#include "scramblerRing.h"
#include "pico/bootrom.h"
#include "hardware/xosc.h"
#include "powerSaving.h"

static u32 sProgramOffset;
FATFS sFatFs;
SdCard gSdCard;
static bool sIsSdCardMounted;

// ROM loaded from SD card to flash
uint32_t gLoadedDefaultRomSize = 0;
uint32_t gLoadedDsiRomSize = 0;
uint32_t gLoadedNtrbootRomSize = 0;
uint32_t gLoadedNtrbootDsiRomSize = 0;

// Flash layout:
// 0x000000 - 0x03EFFF: Firmware (~60 KB actual, 252 KB reserved)
// 0x03F000 - 0x03FFFF: Metadata sector (magic + ROM sizes)
// 0x040000 - 0x0DFFFF: default.nds (640 KB max)
// 0x0E0000 - 0x0EFFFF: ntrboot.nds (64 KB max)
// 0x0F0000 - 0x0FFFFF: ntrbootdsi.nds (64 KB max)
// 0x100000 - 0x1FFFFF: dsimode.nds (1024 KB max)
#define ROM_META_FLASH_OFFSET       ((256 * 1024) - FLASH_SECTOR_SIZE)
#define ROM_META_FLASH_ADDR         (XIP_BASE + ROM_META_FLASH_OFFSET)

#define ROM_DEFAULT_FLASH_OFFSET    (256 * 1024)
#define ROM_DEFAULT_FLASH_ADDR      (XIP_BASE + ROM_DEFAULT_FLASH_OFFSET)
#define ROM_DEFAULT_MAX_SIZE        (640 * 1024)

#define ROM_NTRBOOT_FLASH_OFFSET    (896 * 1024)
#define ROM_NTRBOOT_FLASH_ADDR      (XIP_BASE + ROM_NTRBOOT_FLASH_OFFSET)
#define ROM_NTRBOOT_MAX_SIZE        (64 * 1024)

#define ROM_NTRBOOTDSI_FLASH_OFFSET (960 * 1024)
#define ROM_NTRBOOTDSI_FLASH_ADDR   (XIP_BASE + ROM_NTRBOOTDSI_FLASH_OFFSET)
#define ROM_NTRBOOTDSI_MAX_SIZE     (64 * 1024)

#define ROM_DSI_FLASH_OFFSET        (1024 * 1024)
#define ROM_DSI_FLASH_ADDR          (XIP_BASE + ROM_DSI_FLASH_OFFSET)
#define ROM_DSI_MAX_SIZE            (1024 * 1024)

static uint8_t sFlashBuf[4096] __attribute__((aligned(256)));

#define ROM_META_MAGIC 0x44535043  // "DSPC"

// Only default.nds is required. Others can be 0 (not present).
static bool flashHasValidRom(void)
{
    const uint32_t* meta = (const uint32_t*)ROM_META_FLASH_ADDR;
    if (meta[0] != ROM_META_MAGIC)
        return false;
    uint32_t defaultSize = meta[1];
    if (defaultSize == 0 || defaultSize > ROM_DEFAULT_MAX_SIZE)
        return false;
    return true;
}

static void flashGetStoredRomSizes(void)
{
    const uint32_t* meta = (const uint32_t*)ROM_META_FLASH_ADDR;
    gLoadedDefaultRomSize = meta[1];
    gLoadedDsiRomSize = meta[2];
    gLoadedNtrbootRomSize = meta[3];
    gLoadedNtrbootDsiRomSize = meta[4];
}

#define PIN_LED_RED     27
#define PIN_LED_BLUE    28

static void setRomToDsiRom(void)
{
    if (gLoadedDsiRomSize > 0)
    {
        gNtrRomEmu.romData = gDsiRom;
        gNtrRomEmu.romSize = gLoadedDsiRomSize;
    }
    else
    {
        gNtrRomEmu.romData = gDefaultRom;
        gNtrRomEmu.romSize = gLoadedDefaultRomSize;
    }
    gNtrRomEmu.cardId = CARD_ID_TWL;
    gNtrRomEmu.isDSMode = true;
}

static void resetNtrCard(void)
{
    ntrc_resetUsb();
    pwr_disableAfterBootPowerSaving();
    ntrc_setNormalMode();
    gNtrRomEmu.securePhase1 = false;
    gNtrRomEmu.cmdScramble = false;
    gNtrRomEmu.dataScramble = false;
    gComputeScrambler = false;
    gNtrRomEmu.scrRingRPtr = gScramblerRing;
    gScramblerRingWPtr = gScramblerRing;
    gNtrRomEmu.wordIdx = 0;
    gNtrRomEmu.twlMode = false;
    gNtrRomEmu.readDataDestination = nullptr;
    gNtrRomEmu.readDataCompleteHandler = nullptr;
    gNtrRomEmu.readDataLimit = 0;
#ifdef ENABLE_R4_MODE
    ntrc_resetR4();
#endif
    dma_channel_abort(0);
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_set_pindirs_with_mask(pio0, 0, 0, PIN_INPUT_MASK);
    pio_sm_clear_fifos(pio0, 0);
    pio_sm_restart(pio0, 0);
    pio_sm_clkdiv_restart(pio0, 0);
    irq_clear(PIO0_IRQ_0);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_sm_exec(pio0, 0, pio_encode_jmp(sProgramOffset));
    pio_sm_set_enabled(pio0, 0, true);
    pwr_disableSysTickClock();
    setRomToDsiRom();
    gNtrRomEmu.cardId = 0xC00000C2;
#ifdef DSPICO_ENABLE_WRFUXXED
    ntrc_resetSpiUart();
#endif
}

#ifdef ENABLE_PREVENT_DSI_AUTOBOOT
static u64 sResetStart;
#endif

static void __time_critical_func(gpioIrq)(uint gpio, u32 events)
{
#ifdef ENABLE_PREVENT_DSI_AUTOBOOT
    u64 time = time_us_64();
#endif
    if (gpio == PIN_RST)
    {
        if (events & GPIO_IRQ_EDGE_FALL)
        {
            pio_sm_set_enabled(pio0, 0, false);
            pio_sm_set_pindirs_with_mask(pio0, 0, 0, PIN_INPUT_MASK);
        }
        if (events & GPIO_IRQ_EDGE_RISE)
        {
            resetNtrCard();
        #ifdef ENABLE_PREVENT_DSI_AUTOBOOT
            u32 resetTime = time - sResetStart;
            if (resetTime > 700000)
                pio_sm_set_enabled(pio0, 0, false);
            sResetStart = time;
        #endif
        }   
    }        
}

void __scratch_x("cpu1") core1_entry(void)
{
    irq_set_mask_enabled(~0u, false);
    scb_hw->scr |= M0PLUS_SCR_SLEEPDEEP_BITS;
    while (!gComputeScrambler)
    {
        gScramblerRingWPtr = gScramblerRing;
        __wfe();
    }
    while (1)
    {
        u32* wPtr = gScramblerRingWPtr;
        u32* next = SCR_RING_WRAP(wPtr + 1);
        if (next == gNtrRomEmu.scrRingRPtr)
        {
            __wfe();
            continue;
        }

        *wPtr = scr_getNext32(&gScramblerState);
        gScramblerRingWPtr = next;
    }
}

static void initSd(void)
{
    memset(&sFatFs, 0, sizeof(sFatFs));

    //try mounting 16 times
    bool ok = false;
    for (int i = 0; i < 16; i++)
    {
        FRESULT mountResult = f_mount(&sFatFs, "0:", 1);
        if (mountResult == FR_OK)
        {
            ok = true;
            sIsSdCardMounted = true;
            break;
        }
        else if (mountResult == FR_NO_FILESYSTEM)
        {
            break;
        }
    }
    if (!ok)
    {
        sIsSdCardMounted = false;
    }
}

static void tryRebootToBootsel(void)
{
    if (!sIsSdCardMounted)
    {
        xosc_init();
        reset_usb_boot(0, 0);
    }
}

// ---- Flash programming for SD-loaded ROM ----

// Must run from RAM - flash is inaccessible during erase/program
static void __no_inline_not_in_flash_func(flashEraseSector)(uint32_t offset)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(flashProgramChunk)(uint32_t offset, const uint8_t* data, uint32_t len)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, len);
    restore_interrupts(ints);
}

static void flashWriteMetadata(void)
{
    memset(sFlashBuf, 0xFF, FLASH_SECTOR_SIZE);
    uint32_t magic = ROM_META_MAGIC;
    memcpy(sFlashBuf, &magic, sizeof(magic));
    memcpy(sFlashBuf + 4, &gLoadedDefaultRomSize, sizeof(gLoadedDefaultRomSize));
    memcpy(sFlashBuf + 8, &gLoadedDsiRomSize, sizeof(gLoadedDsiRomSize));
    memcpy(sFlashBuf + 12, &gLoadedNtrbootRomSize, sizeof(gLoadedNtrbootRomSize));
    memcpy(sFlashBuf + 16, &gLoadedNtrbootDsiRomSize, sizeof(gLoadedNtrbootDsiRomSize));
    flashEraseSector(ROM_META_FLASH_OFFSET);
    flashProgramChunk(ROM_META_FLASH_OFFSET, sFlashBuf, 256);
}

static bool loadRomToFlash(const char* filename, uint32_t flashOffset, uint32_t maxSize, uint32_t* sizeOut)
{
    FIL fil;
    FRESULT res = f_open(&fil, filename, FA_READ);
    if (res != FR_OK)
        return false;

    FSIZE_t fileSize = f_size(&fil);
    if (fileSize == 0 || fileSize > maxSize)
    {
        f_close(&fil);
        return false;
    }

    uint32_t alignedSize = (uint32_t)((fileSize + 511) & ~511);

    // Quick check: compare first 512 bytes to see if flash already matches
    UINT bytesRead;
    res = f_read(&fil, sFlashBuf, 512, &bytesRead);
    if (res != FR_OK || bytesRead != 512)
    {
        f_close(&fil);
        return false;
    }

    const uint8_t* flashRom = (const uint8_t*)(XIP_BASE + flashOffset);
    if (memcmp(sFlashBuf, flashRom, 512) == 0)
    {
        // Flash already has the right data, skip reprogramming
        f_close(&fil);
        *sizeOut = alignedSize;
        return true;
    }

    // ROM has changed - reprogram flash
    f_lseek(&fil, 0);

    uint32_t offset = 0;
    uint32_t blinkCounter = 0;
    while (offset < fileSize)
    {
        UINT toRead = fileSize - offset;
        if (toRead > FLASH_SECTOR_SIZE)
            toRead = FLASH_SECTOR_SIZE;

        res = f_read(&fil, sFlashBuf, toRead, &bytesRead);
        if (res != FR_OK || bytesRead == 0)
            break;

        // Pad to 256-byte boundary for flash programming
        uint32_t padded = (bytesRead + 255) & ~255;
        if (padded > bytesRead)
            memset(sFlashBuf + bytesRead, 0xFF, padded - bytesRead);

        flashEraseSector(flashOffset + offset);
        flashProgramChunk(flashOffset + offset, sFlashBuf, padded);

        // Blink red LED during programming
        gpio_put(PIN_LED_RED, (++blinkCounter) & 1);

        offset += bytesRead;
    }

    gpio_put(PIN_LED_RED, 0);
    f_close(&fil);
    *sizeOut = alignedSize;
    return true;
}

static bool loadRomsFromSd(void)
{
    bool defaultOk = loadRomToFlash("default.nds", ROM_DEFAULT_FLASH_OFFSET, ROM_DEFAULT_MAX_SIZE, &gLoadedDefaultRomSize);
    // Optional ROMs — size stays 0 if file not found
    loadRomToFlash("dsimode.nds", ROM_DSI_FLASH_OFFSET, ROM_DSI_MAX_SIZE, &gLoadedDsiRomSize);
    loadRomToFlash("ntrboot.nds", ROM_NTRBOOT_FLASH_OFFSET, ROM_NTRBOOT_MAX_SIZE, &gLoadedNtrbootRomSize);
    loadRomToFlash("ntrbootdsi.nds", ROM_NTRBOOTDSI_FLASH_OFFSET, ROM_NTRBOOTDSI_MAX_SIZE, &gLoadedNtrbootDsiRomSize);

    if (defaultOk)
    {
        flashWriteMetadata();
        gpio_put(PIN_LED_BLUE, 1);
        return true;
    }

    return false;
}

// ---- Cart protocol setup ----

static void setupCartProtocol(void)
{
    gNtrRomEmu.cardId = CARD_ID_TWL;
    setRomToDsiRom();
    gNtrRomEmu.isDSMode = true;

    sProgramOffset = pio_add_program(pio0, &ntr_card_program);
#ifdef DSPICO_ENABLE_WRFUXXED
    u32 spiUartProgOffs = pio_add_program(pio0, &ntr_card_spi_program);
#endif
    pio_sm_config c = ntr_card_program_get_default_config(sProgramOffset);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_sideset_pins(&c, PIN_D5);
    sm_config_set_set_pins(&c, PIN_D0, 5);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv(&c, 1);
    pio_sm_set_pindirs_with_mask(pio0, 0, 0, PIN_INPUT_MASK);
    pio_sm_set_pins_with_mask(pio0, 0, 0, PIN_INPUT_MASK);
    pio0->input_sync_bypass = 0xFF000;
    pio_gpio_init(pio0, PIN_D0);
    pio_gpio_init(pio0, PIN_D1);
    pio_gpio_init(pio0, PIN_D2);
    pio_gpio_init(pio0, PIN_D3);
    pio_gpio_init(pio0, PIN_D4);
    pio_gpio_init(pio0, PIN_D5);
    pio_gpio_init(pio0, PIN_D6);
    pio_gpio_init(pio0, PIN_D7);

    pio_sm_init(pio0, 0, sProgramOffset, &c);
    pio_set_irq0_source_enabled(pio0, pis_sm0_rx_fifo_not_empty, true);
    irq_set_exclusive_handler(PIO0_IRQ_0, ntrc_pioIrq);
#ifdef DSPICO_ENABLE_WRFUXXED
    ntrc_initSpiUart(spiUartProgOffs);
#endif

    gpio_set_irq_callback(gpioIrq);
    gpio_set_irq_enabled(PIN_RST, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);

    irq_init_priorities();
    // Setup the systick timer with the processor clock as clock source (200MHz = 5ns resolution)
    systick_hw->csr = 0x5;
    systick_hw->rvr = 0x00ffffff;
    irq_set_priority(PIO0_IRQ_0, 0x40);
    irq_set_priority(IO_IRQ_BANK0, 0x40);
    irq_set_priority(USBCTRL_IRQ, 0x80);
    irq_set_priority(DMA_IRQ_1, 0x80);
    irq_set_priority(TIMER_IRQ_0, 0x80);

    resetNtrCard();
}

// ---- Main ----

int __time_critical_func(main)()
{
    bi_decl(bi_program_description("Ntr card emulator"));
    bi_decl(bi_pin_mask_with_name(0xFF000, "Ntr card D0-D7"));
    bi_decl(bi_1pin_with_name(PIN_IRQ, "Ntr card irq"));
    bi_decl(bi_1pin_with_name(PIN_CEB, "Ntr card ceb (rom enable)"));
    bi_decl(bi_1pin_with_name(PIN_WREB, "Ntr card wreb (clock)"));
    bi_decl(bi_1pin_with_name(PIN_RST, "Ntr card reset"));
    bi_decl(bi_1pin_with_name(PIN_CS2, "Ntr card cs2 (spi enable)"));

    // 200 MHz = 1200 MHz / 6 / 1
    set_sys_clock_pll(1200000000, 6, 1);

    dma_channel_claim(0);

    memset(&gNtrRomEmu, 0, sizeof(gNtrRomEmu));

    multicore_launch_core1(core1_entry);

    // GPIO init
    gpio_init_mask(PIN_INPUT_MASK);
    gpio_set_dir_in_masked(PIN_INPUT_MASK);
    gpio_init(PIN_IRQ);
    gpio_put(PIN_IRQ, 0);
    gpio_set_dir(PIN_IRQ, GPIO_OUT);
    gpio_disable_pulls(PIN_D0);
    gpio_disable_pulls(PIN_D1);
    gpio_disable_pulls(PIN_D2);
    gpio_disable_pulls(PIN_D3);
    gpio_disable_pulls(PIN_D4);
    gpio_disable_pulls(PIN_D5);
    gpio_disable_pulls(PIN_D6);
    gpio_disable_pulls(PIN_D7);
    gpio_set_slew_rate(PIN_D0, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D1, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D2, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D3, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D4, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D5, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D6, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_D7, GPIO_SLEW_RATE_FAST);
    gpio_pull_up(PIN_CEB);
    gpio_pull_up(PIN_WREB);
    gpio_pull_down(PIN_RST);
    gpio_pull_up(PIN_CS2);
    gpio_set_drive_strength(PIN_D0, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D1, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D2, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D3, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D4, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D5, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D6, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(PIN_D7, GPIO_DRIVE_STRENGTH_2MA);

    // Initialize status LEDs
    gpio_init(PIN_LED_RED);
    gpio_init(PIN_LED_BLUE);
    gpio_set_dir(PIN_LED_RED, GPIO_OUT);
    gpio_set_dir(PIN_LED_BLUE, GPIO_OUT);
    gpio_put(PIN_LED_RED, 0);
    gpio_put(PIN_LED_BLUE, 0);

    // FAST PATH: ROM already in flash from previous boot
    if (flashHasValidRom())
    {
        flashGetStoredRomSizes();
       // gpio_put(PIN_LED_BLUE, 1);
        setupCartProtocol();

        // Single SD init for game-time access
        sIsSdCardMounted = false;
        initSd();
        tryRebootToBootsel();
    }
    else
    {
        // COLD PATH: no ROM in flash, must load from SD first (requires pre-power)
        sIsSdCardMounted = false;
        initSd();

        if (!sIsSdCardMounted)
        {
            xosc_init();
            reset_usb_boot(0, 0);
        }

        loadRomsFromSd();
        setupCartProtocol();
    }

    pwr_initPowerSaving();

    while (1)
    {
        gSdCard.Update();
        gSdCard.Update();
    #ifdef ENABLE_R4_MODE
        ntrc_gameR4Update();
    #endif
        __wfi();
    }
}
