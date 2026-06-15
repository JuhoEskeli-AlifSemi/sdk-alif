/*
 * Place USB host DMA buffers in SRAM0 (bus-accessible by USB DMA).
 * DTCM is M55-local and not reachable by the USB controller's DMA engine.
 */

SECTION_DATA_PROLOGUE(.sram0.bss.uhc_dma, (NOLOAD),)
{
	. = ALIGN(64);
	*(.sram0.bss.uhc_dma)
	*(.sram0.bss.uhc_dma.*)
	. = ALIGN(64);
} GROUP_DATA_LINK_IN(SRAM0, SRAM0)
