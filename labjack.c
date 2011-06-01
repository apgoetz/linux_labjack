/* 
 * Written by Andy Goetz
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

#define LJ_VENDOR_ID  0x0CD5
#define LJ_PRODUCT_ID 0x0003

#define LJ_NUM_MINORS 3		/* minor char devices per lj */
#define MAXDEV 8		/* max number of connected ljs */
#define MINOR_START 135		/* start minor number */
#define LJ_NAMESIZE 20		/* 20 char max for name. */
/* keeps track of usb interfaces that are connected */
static struct lj_state **lj_state_table = NULL;

/* this lock protects the character device registering from stepping
   on the allocation of the numbers. */
static struct mutex state_table_lock;


static ssize_t bchr_read(struct file *file, char __user *buf, 

		  size_t size, loff_t *off);

static int chr_open(struct inode *inode, struct file *file);


static ssize_t cchr_read(struct file *file, char __user *buf, 
		  size_t size, loff_t *off);



static int lj_probe(struct usb_interface *intf, const struct usb_device_id *id);

static void lj_disconnect(struct usb_interface *intf);

static struct file_operations bchr_ops = {
  .owner = THIS_MODULE,
  .read = bchr_read,
  .open = chr_open,
};


static struct file_operations cchr_ops = {
  .owner = THIS_MODULE,
  .read = cchr_read,
  .open = chr_open,
};


struct lj_state {
  /* used to sling messages around through the USB. */
  struct usb_device *usb_device;
  /* prevents multiple hardware requests at once, per labjack. */
  struct mutex *hw_lock;
  
  struct miscdevice achr_device;
  struct miscdevice bchr_device;
  struct miscdevice cchr_device;
};

static struct usb_device_id id_table [] = {
  {  USB_DEVICE(LJ_VENDOR_ID, LJ_PRODUCT_ID) },
  { }
};


static struct usb_driver usb_driver = {
  /* .owner = THIS_MODULE, */
  .name = "labjack",
  .id_table = id_table,
  .probe = lj_probe,
  .disconnect = lj_disconnect,
};

static void print_arr(u8* data, int size)
{
  int i;
  printk("[ ");
  for(i = 0; i < size - 1; i++)
    printk("0x%x, ", data[i]);
  printk("0x%x ]", data[size - 1]);
}

static void fix_checksum8(u8* packet, u16 size)
{
  u16 acc = 0;
  u16 i;
  for( i = 1; i < size; i++)
    {
      acc += packet[i];
    }
  acc = (acc & 0xff) + (acc >> 8);
  acc = (acc & 0xff) + (acc >> 8);

  *packet = (u8)(acc & 0xff);
}

static int insert_state_table(struct lj_state *state)
{
  int i;
  
  mutex_lock(&state_table_lock);

  for(i = 0; i < MAXDEV; i++)
    {
      if(!lj_state_table[i])
	{
	  lj_state_table[i] = state;
	  mutex_unlock(&state_table_lock);
	  return i*LJ_NUM_MINORS + MINOR_START;
	}
    }
  /* if we have gotten here, there is no more room to register a device. */
  mutex_unlock(&state_table_lock);
  return -1;
  
}

static int remove_state_table(int minor)
{
  /* this gives us an integer that is an index into the interface
     table based on the minor number used. */
  int index = (minor - MINOR_START) / LJ_NUM_MINORS;
  if(index >= MAXDEV)
    {
      printk(KERN_INFO "someone passed in an invalid minor number: %d\n",
	     minor);
      return -1;
    }
  mutex_lock(&state_table_lock);
  if(lj_state_table[index] == NULL)
    {
      printk(KERN_INFO "device is already NULL! cannot remove from" 
	     "interface table.\n");
      mutex_unlock(&state_table_lock);
      return -1;
    }
  lj_state_table[index] = NULL;
  mutex_unlock(&state_table_lock);
  return 0;  
}

static struct lj_state* get_lj_state(int minor)
{
  /* this gives us an integer that is an index into the interface
     table based on the minor number used. */
  int index = (minor - MINOR_START) / LJ_NUM_MINORS;
  struct lj_state *state;
  if(index >= MAXDEV)
    {
      printk(KERN_INFO "someone passed in an invalid minor number: %d\n",
	     minor);
      return NULL;
    }
  mutex_lock(&state_table_lock);
  state = lj_state_table[index]; 
  mutex_unlock(&state_table_lock);
  return state;
}

static void fix_checksum16(u8* packet, u16 size)
{
  int i;
  u16 acc = 0;
  for (i = 6; i < size; i++)
    {
      acc += packet[i];
    }
  packet[4] = (u8)(acc & 0xff);
  packet[5] = (u8)(acc >> 8);

  fix_checksum8(packet, 6);

}


static  int lj_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
  
  

  struct usb_device *usb_device;
  
  struct lj_state *lj_state = NULL;

  int result;
  const int CFGSIZE = 12;
  const int RCVSIZE = 12;
  u8 config_packet[CFGSIZE];
  u8 rcv_packet[RCVSIZE];
  int sent_len;
  int minor;
  int devid;
  char *tmpname = NULL;
  /* 8bit checksum */
  config_packet[1] = 0xf8; 	/* ConfigIO packet */
  config_packet[2] = 0x03;
  config_packet[3] = 0x0b;
  /* 16bit checksum */
  config_packet[6] = 15;	/* set everything */
  config_packet[7] = 0x00;	/* reserved */
  config_packet[8] = 0x00;	/* no timers */
  config_packet[9] = 0x00;	/* deprecated */
  config_packet[10] = 0x00;	/* no Analog on FIO */
  config_packet[12] = 0x02;	/* EIO2 is AIN9 */
  
  fix_checksum16(config_packet, CFGSIZE);
  
  printk(KERN_INFO "You were probed!!!\n");

  lj_state = kzalloc(sizeof(struct lj_state), GFP_KERNEL);
  
  if(!lj_state)
    {
      printk( KERN_INFO "Could not allocate memory for labjack state!!!\n");
      goto error;
    }
  
  usb_device = interface_to_usbdev(intf);
  
  lj_state->usb_device = usb_device;

  lj_state->hw_lock = NULL;
  lj_state->hw_lock = kmalloc(sizeof(struct mutex), GFP_KERNEL);
  
  if(lj_state->hw_lock == NULL)
    {
      printk(KERN_INFO "Could not allocate memory for hardware mutex!\n");
      goto err_free;
    }
  
  mutex_init(lj_state->hw_lock);
  
  usb_set_intfdata(intf, lj_state);
  
  result = usb_bulk_msg(lj_state->usb_device, 
			usb_sndbulkpipe(lj_state->usb_device, 1),
			config_packet, CFGSIZE, &sent_len, 5);
  

  if(result)
    {
      printk("Could not send bulk message to configure IO.\n");
      goto err_hwlock;
    }

  result = usb_bulk_msg(lj_state->usb_device, 
			usb_rcvbulkpipe(lj_state->usb_device, 2),
			rcv_packet, RCVSIZE, &sent_len, 5);
  if(result)
  {
    printk("Could not receive bulk message to configure IO.\n");
    goto err_hwlock;
  }
  if(rcv_packet[0] == 0xb8 && rcv_packet[1] == 0xb8)
    {
          printk("We got a bad checksum. Orig packet was:\n");
	  print_arr(config_packet, CFGSIZE);
	  printk("\n");
	  goto err_hwlock;
    }
  printk(KERN_INFO "EIN2 configure as AIN9\n");
  
  minor = insert_state_table(lj_state);
  if(minor < 0)
    {
      printk(KERN_INFO "could not add usb_interface to interface table!\n");
      goto err_hwlock;
    }

  devid = minor - MINOR_START;
  tmpname = kmalloc(sizeof(char)*LJ_NAMESIZE, GFP_KERNEL);
  sprintf(tmpname, "lab%dportB",devid);
  lj_state->bchr_device.name = tmpname;
  lj_state->bchr_device.minor = minor + 1; /* b's minor is start minor + 1 */
  lj_state->bchr_device.fops = &bchr_ops;


  result = misc_register(&lj_state->bchr_device);
  
  if(result)
    {
      printk( KERN_INFO "Could not register portb.\n");
      goto err_intf;
    }
  else
    {
      printk( KERN_INFO "Registered a portb char dev!\n");
    }


  tmpname = kmalloc(sizeof(char)*LJ_NAMESIZE, GFP_KERNEL);
  sprintf(tmpname, "lab%dportC",devid);
  lj_state->cchr_device.name = tmpname;
  lj_state->cchr_device.minor = minor + 2; /* c's minor is start minor + 1 */
  lj_state->cchr_device.fops = &cchr_ops;


  result = misc_register(&lj_state->cchr_device);
  
  if(result)
    {
      printk( KERN_INFO "Could not register portc.\n");
      goto err_regb;
    }
  else
    {
      printk( KERN_INFO "Registered a portc char dev!\n");
    }



  return 0;
  
 err_regb:
  misc_deregister(&lj_state->bchr_device);
  kfree(lj_state->bchr_device.name);
 err_intf:
  remove_state_table(minor);
 err_hwlock:
  mutex_destroy(lj_state->hw_lock);
  kfree(lj_state->hw_lock);
 err_free:
  usb_set_intfdata(intf, NULL);
  kfree(lj_state);
  
 error:
  return -1;
}

static  void lj_disconnect(struct usb_interface *intf)
{
  struct lj_state *curstate;
  
  int minor;
  
  printk(KERN_INFO "ByeBye HW!!!\n");
  
  curstate = usb_get_intfdata(intf);
  
  minor = curstate->bchr_device.minor;
  remove_state_table(minor);
  
  
  misc_deregister(&curstate->bchr_device);
  kfree(curstate->bchr_device.name);
  
  misc_deregister(&curstate->cchr_device);
  kfree(curstate->cchr_device.name);

  kfree(curstate);
  usb_set_intfdata(intf, NULL);
    
}
static int chr_open(struct inode *inode, struct file *file)
{
  struct lj_state *lj_state = NULL;
  int subminor;

  subminor = iminor(inode);
  
  
  lj_state = get_lj_state(subminor);
  if (!lj_state)
    {
      printk(KERN_INFO "Could not access labjack state!\n");
      goto error;
    }
  
  file->private_data = lj_state;
  printk(KERN_INFO "someone opened me!\n");
  return 0;
 error:
  return -1;
}

static ssize_t bchr_read(struct file *file, char __user *buf, 
		  size_t size, loff_t *off)
{
  const int SNDSIZE = 10;
  const int KFROMBIN = 13;
  const int KDIV = 1000;
  const int RCVSIZE = 12;
  struct lj_state *lj_state = NULL;
  int sent_len;
  int rawtemp;
  int scaledtemp;
  u8 snd_packet[SNDSIZE];
  u8 rcv_packet[RCVSIZE];
  int result;

  /* if they don't give us enough space, we have to abort*/
  if(size < sizeof (int))
    return -EINVAL;
  
  /* 8bit checksum */
  snd_packet[1] = 0xf8;
  snd_packet[2] = 0x2; 		/* number of words is .5 + 1.5 */
  snd_packet[3] = 0x00;
  /* 16bit checksum */
  snd_packet[6] = 0x00;		/* echo can be whatever we want */
  snd_packet[7] = 0x01;		/* Do an analog in */
  snd_packet[8] = 30;		/* read the temp */
  snd_packet[9] = 31;		/* compare it to gnd */

  fix_checksum16(snd_packet, SNDSIZE);

  lj_state = file->private_data;
  
  printk(KERN_INFO "Someone tried to read on portb!\n"
	 "devnum: %d\n", lj_state->usb_device->devnum);

  
  mutex_lock(lj_state->hw_lock);
  
  /* in here, this function has unique access to the hardware. */


  result = usb_bulk_msg(lj_state->usb_device, 
	       usb_sndbulkpipe(lj_state->usb_device, 1),
	       snd_packet, SNDSIZE, &sent_len, 5);
  
  if(result){
      printk(KERN_INFO "Could not successfully send bulk message!\n");
      goto error;
    }
  else{
    printk("Successfully sent bulk message!\n");
  }


  result = usb_bulk_msg(lj_state->usb_device, 
	       usb_rcvbulkpipe(lj_state->usb_device, 2),
	       rcv_packet, RCVSIZE, &sent_len, 5);
  
  if(result){
      printk(KERN_INFO "Could not successfully receive bulk message!\n");
      goto error;
    }
  else{
    printk("Successfully received bulk message!\n"
	   "It was %d bytes. \n", sent_len);
  }

  if(rcv_packet[6])
    {
      printk("but there was an error! data was:\n");
      print_arr(rcv_packet, RCVSIZE);
      printk("\n");
      goto error;
    }
  
  rawtemp = rcv_packet[9] + (rcv_packet[10] << 8);
  scaledtemp = (rawtemp * KFROMBIN) / KDIV;
  scaledtemp -= 273; 		/* convert to celsius */
  printk("Temp is %d C.\n", scaledtemp);
  mutex_unlock(lj_state->hw_lock);
  
  copy_to_user(buf, &scaledtemp, sizeof(int));
  
  return sizeof(int);
  
 error:
  mutex_unlock(lj_state->hw_lock);
  return -EINVAL;
}


static ssize_t cchr_read(struct file *file, char __user *buf, 
		  size_t size, loff_t *off)
{
  printk(KERN_INFO "Someone tried to read on portC!\n");
  return -EINVAL;
}







static int __init lj_start(void)
{
  int result = 0;

  printk(KERN_INFO "Hello, kernel!\n");
  
  lj_state_table = kzalloc(sizeof(struct usb_interface*) * MAXDEV, GFP_KERNEL);
  
  if(!lj_state_table)
    {
      printk(KERN_INFO "Could not allocate memory for interface table!\n");
      goto error;
    }


  result =  usb_register(&usb_driver);
  if (result)
    {
      printk(KERN_INFO "Could not register device: %d", result);
      goto error_reg;
    }
  mutex_init(&state_table_lock);
  return 0;
 error_reg:
  kfree(lj_state_table);
 error:
  return -1;
}



static void __exit lj_end(void)
{

  usb_deregister(&usb_driver);
  kfree(lj_state_table);
  mutex_destroy(&state_table_lock);
  printk(KERN_INFO "Goodbye, kernel!\n");
}

MODULE_AUTHOR("Andy Goetz");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
module_init(lj_start);
module_exit(lj_end);

