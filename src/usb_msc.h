#ifndef USB_MSC_H_
#define USB_MSC_H_

/*
 * USB Mass Storage offload. Mutually exclusive with logging at the FS level
 * (REQUIREMENTS.md section 6): unmount FatFs before exposing the card as MSC.
 */

int usb_storage_init(void);
int usb_offload_enter(void);  /* stop logging, unmount FS, expose MSC */
int usb_offload_exit(void);   /* hide MSC, remount FS */

#endif /* USB_MSC_H_ */
