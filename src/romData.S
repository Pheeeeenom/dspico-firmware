.section .flashdata,"a"

@ All ROMs loaded from SD card to flash at runtime.
.global gDefaultRom
.equ gDefaultRom, (0x10000000 + 0x040000)

.global gNtrbootRom
.equ gNtrbootRom, (0x10000000 + 0x0E0000)

.global gNtrbootDsiRom
.equ gNtrbootDsiRom, (0x10000000 + 0x0F0000)

.global gDsiRom
.equ gDsiRom, (0x10000000 + 0x100000)

.end
