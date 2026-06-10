/* Linker script to configure memory regions — nRF52840 + S340 v7.0.1.
 *
 * Derived from the stock Adafruit BSP `nrf52840_s140_v6.ld`. Only the FLASH
 * region changes: the S340 SoftDevice is larger than S140, so the application
 * starts higher and ends below our custom S340 bootloader.
 *
 *   FLASH ORIGIN = 0x31000  = MBR 0x1000 + SD_FLASH_SIZE 0x30000
 *                             (S340 7.0.1 nrf_sdm.h:144)
 *   FLASH END    = 0xF4000  = release BOOTLOADER_REGION_START of the custom
 *                             S340 bootloader (sibling repo Makefile; the
 *                             0xEA000 there is DEBUG-only)
 *
 * RAM ORIGIN (0x20006000) is a starting reservation. S340 with ANT enabled may
 * require more RAM than S140; if the boot log reports a SoftDevice RAM
 * requirement above 0x20006000, raise ORIGIN to the printed value, re-run the
 * installer, and re-flash.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x31000, LENGTH = 0xF4000 - 0x31000

  /* SRAM required by Softdevice depend on
   * - Attribute Table Size (Number of Services and Characteristics)
   * - Vendor UUID count
   * - Max ATT MTU
   * - Concurrent connection peripheral + central + secure links
   * - Event Len, HVN queue, Write CMD queue
   * - ANT channels (S340)
   */
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
