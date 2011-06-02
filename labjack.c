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
#include <linux/timer.h>
#include <linux/wait.h>

#define LJ_VENDOR_ID  0x0CD5
#define LJ_PRODUCT_ID 0x0003

#define LJ_NUM_MINORS 3		/* minor char devices per lj */
#define MAXDEV 8		/* max number of connected ljs */
#define MINOR_START 135		/* start minor number */
#define LJ_NAMESIZE 20		/* 20 char max for name. */
#define LJ_PORTC_FREQ (HZ*1)	/* frequency with which to check the airlock */

/* keeps track of usb interfaces that are connected */
static struct lj_state **lj_state_table = NULL;

/* this lock protects the character device registering from stepping
   on the allocation of the numbers. */
static struct mutex state_table_lock;


static ssize_t bchr_read(struct file *file, char __user *buf, 

			size_t size, loff_t *off);

static int chr_open(struct inode *inode, struct file *file);


static ssize_t achr_read(struct file *file, char __user *buf, 
			size_t size, loff_t *off);


static ssize_t cchr_read(struct file *file, char __user *buf, 
			size_t size, loff_t *off);



static int lj_probe(struct usb_interface *intf, const struct usb_device_id *id);

static void lj_disconnect(struct usb_interface *intf);

static struct file_operations achr_ops = {
	.owner = THIS_MODULE,
	.read = achr_read,
	.open = chr_open,
};


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

enum airlock_state {air_open, air_closed, air_error};

struct lj_state {
	/* used to sling messages around through the USB. */
	struct usb_device *usb_device;
	/* prevents multiple hardware requests at once, per labjack. */
	struct mutex *hw_lock;
	/* miscdevice struct for portA */
	struct miscdevice achr_device;
	/* miscdevice struct for portB */
	struct miscdevice bchr_device;
	/* miscdevice struct for portC */
	struct miscdevice cchr_device;
	/* timer used by portC to check EIN2's voltage at 1Hz */
	struct timer_list c_poll_timer;
	/* waitqueue for portC processes that are blocking */
	wait_queue_head_t c_waitqueue;
	/* this flag is used to determine whether or not the read
	 * calls on portC will unblock. Because only one person ever
	 * writes to this location, we don't need to lock it. */
	enum airlock_state  airlock;
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


static int was_err(u8 *buf, int len)
{
	if (len == 2 &&
		buf[0] == 0xb8 &&
		buf[1] == 0xb8){
		return -1;
	}
	return 0;
}

/* debug function to print an array to /var/log/messages. This is done
   using */
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
	for( i = 1; i < size; i++){
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
	
	for(i = 0; i < MAXDEV; i++){
		if(!lj_state_table[i]){
			lj_state_table[i] = state;
			mutex_unlock(&state_table_lock);
			return i*LJ_NUM_MINORS + MINOR_START;
		}
	}
	/* if we have gotten here, there is no more room to register a
	 * device. */
	mutex_unlock(&state_table_lock);
	return -1;
  
}

static int remove_state_table(int minor)
{
	/* this gives us an integer that is an index into the interface
	   table based on the minor number used. */
	int index = (minor - MINOR_START) / LJ_NUM_MINORS;
	if(index >= MAXDEV){
		printk(KERN_INFO 
			"someone passed in an invalid minor number: %d\n",
			minor);
		return -1;
	}
	mutex_lock(&state_table_lock);
	if(lj_state_table[index] == NULL){
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
	if(index >= MAXDEV){
		printk(KERN_INFO 
			"someone passed in an invalid minor number: %d\n",
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
	for (i = 6; i < size; i++){
		acc += packet[i];
	}
	packet[4] = (u8)(acc & 0xff);
	packet[5] = (u8)(acc >> 8);

	fix_checksum8(packet, 6);

}

static void c_urb_in_cbk(struct urb *urb)
{
	int rawvoltage;
	u8 *rcv_packet;
	struct lj_state *curstate;

	if(urb->status && 
		(urb->status == -ENOENT ||
			urb->status == -ECONNRESET ||
			urb->status == -ESHUTDOWN)){			
		printk(KERN_INFO "unexpected urb unlink in portc IN cbk.\n");
		/* for some reason we got shutdown. abort. */
		return;
	}
	else if (urb->status){
		printk(KERN_INFO "Error in portc urb IN cbk: %d.\n", 
			urb->status);
		return;
	}
	if (was_err(urb->transfer_buffer, urb->actual_length))
	{
		printk(KERN_INFO "There was a checksum error!\n");
		goto error;
	}

	rcv_packet = (u8*)urb->transfer_buffer;
	
	if (rcv_packet[6]){
		printk(KERN_INFO "There was an error: %d\n", rcv_packet[6]);
		goto error;
	}


	curstate = (struct lj_state*)urb->context;

	printk(KERN_INFO "Successfully submitted portC IN URB\n");
	
	rawvoltage = rcv_packet[9] + (rcv_packet[10] << 8);

	if(rawvoltage > 26860){
		printk(KERN_INFO "EIN2 greater than 1V\n");
		curstate->airlock = air_open;
		wake_up_interruptible(&curstate->c_waitqueue);
	}
	else{
		printk(KERN_INFO "EIN2 less than 1V\n");
		curstate->airlock = air_closed;
	}
	
error:
	kfree(urb->transfer_buffer);
	usb_free_urb(urb);
	return;

}

static void c_urb_out_cbk(struct urb *urb)
{
	u8 *rcv_packet;
	struct lj_state *curstate;
	const int RCVSIZE = 12;
	int result;
	if(urb->status && 
		(urb->status == -ENOENT ||
			urb->status == -ECONNRESET ||
			urb->status == -ESHUTDOWN)){			
		printk(KERN_INFO "unexpected urb unlink in portc callback.\n");
		/* for some reason we got shutdown. abort. */
		return;
	}
	else if (urb->status){
		printk(KERN_INFO "Error in portc urb out cbk: %d.\n", 
			urb->status);
		return;
	}
	
	/* if we are here, we are go for an in URB */
	printk(KERN_INFO "Successfully submitted portC OUT URB\n");
	curstate = (struct lj_state*)urb->context;
	
	rcv_packet = kmalloc(sizeof(u8)*RCVSIZE, GFP_ATOMIC);

	
	kfree(urb->transfer_buffer);
	usb_fill_bulk_urb(urb, curstate->usb_device, 
			usb_rcvbulkpipe(curstate->usb_device, 2), 
			rcv_packet, RCVSIZE, c_urb_in_cbk, curstate);
	
	/* submit the urb */
	result = usb_submit_urb(urb, GFP_ATOMIC);
	WARN_ON(result);
	return;
}

static void c_timer_cbk(unsigned long state)
{
	struct lj_state *curstate = (struct lj_state*)state;

	const int SNDSIZE = 10;
	u8 *snd_packet = NULL;
	
	int result = 0;
	struct urb *urb;
	

	printk(KERN_INFO "portC polling timer triggered!\n");
	snd_packet = kzalloc(sizeof(u8)*SNDSIZE, GFP_ATOMIC);
	if(!snd_packet)
	{
		printk(KERN_INFO "Could not allocate memory for snd_packet"
			" for portC.\n");
		return;
	}

	/* 8bit checksum */
	snd_packet[1] = 0xf8;
	snd_packet[2] = 0x2; 		/* number of words is .5 + 1.5 */
	snd_packet[3] = 0x00;
	/* 16bit checksum */
	snd_packet[6] = 0x00;		/* echo can be whatever we want */
	snd_packet[7] = 0x01;		/* Do an analog in */
	snd_packet[8] = 10;		/* read AIN10 */
	snd_packet[9] = 31;		/* compare it to gnd */

	fix_checksum16(snd_packet, SNDSIZE);

	
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	urb->transfer_flags = 0;
	usb_fill_bulk_urb(urb, curstate->usb_device, 
			usb_sndbulkpipe(curstate->usb_device, 1), 
			snd_packet, SNDSIZE, c_urb_out_cbk, curstate);

	result = usb_submit_urb(urb, GFP_ATOMIC);
	WARN_ON(result);
	
	/* set up the next interrupt */
	curstate->c_poll_timer.expires += LJ_PORTC_FREQ;
	add_timer(&curstate->c_poll_timer);
	return;
}

static  int lj_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
  
	struct usb_device *usb_device;
  
	struct lj_state *curstate = NULL;

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
	config_packet[8] = 0x40;	/* offset must be at least 4*/
	config_packet[9] = 0x00;	/* deprecated */
	config_packet[10] = 0x00;	/* no Analog on FIO */
	config_packet[11] = 0x04;	/* EIO2 is AIN10 */
  

	fix_checksum16(config_packet, CFGSIZE);

	printk(KERN_INFO "You were probed!!!\n");

	curstate = kzalloc(sizeof(struct lj_state), GFP_KERNEL);
  
	if(!curstate){
		printk( KERN_INFO 
			"Could not allocate memory for labjack state!!!\n");
		goto error;
	}
  
	usb_device = interface_to_usbdev(intf);
  
	curstate->usb_device = usb_device;

	curstate->hw_lock = NULL;
	curstate->hw_lock = kmalloc(sizeof(struct mutex), GFP_KERNEL);
  
	if(curstate->hw_lock == NULL){
		printk(KERN_INFO 
			"Could not allocate memory for hardware mutex!\n");
		goto err_free;
	}
  

	mutex_init(curstate->hw_lock);
	
	init_waitqueue_head(&curstate->c_waitqueue);
	
	usb_set_intfdata(intf, curstate);
  
	result = usb_bulk_msg(curstate->usb_device, 
			usb_sndbulkpipe(curstate->usb_device, 1),
			config_packet, CFGSIZE, &sent_len, 5);
  

	if(result){
		printk("Could not send bulk message to configure IO.\n");
		goto err_hwlock;
	}

	result = usb_bulk_msg(curstate->usb_device, 
			usb_rcvbulkpipe(curstate->usb_device, 2),
			rcv_packet, RCVSIZE, &sent_len, 5);
	if(result){
		printk("Could not receive bulk message to configure IO.\n");
		goto err_hwlock;
	}
	if(rcv_packet[0] == 0xb8 && rcv_packet[1] == 0xb8){
		printk("We got a bad checksum. Orig packet was:\n");
		print_arr(config_packet, CFGSIZE);
		printk("\n");
		goto err_hwlock;
	}
	if(rcv_packet[6])
	{
		printk("error in configio: %d\n", rcv_packet[6]);
		goto err_hwlock;
	}
  
	minor = insert_state_table(curstate);
	if(minor < 0){
		printk(KERN_INFO
			"could not add usb_interface to interface table!\n");
		goto err_hwlock;
	}

	devid = minor - MINOR_START;
	
	/* create the portC timer callback */
	init_timer(&curstate->c_poll_timer);

	curstate->c_poll_timer.expires = jiffies + LJ_PORTC_FREQ;
	curstate->c_poll_timer.function = c_timer_cbk;
	curstate->c_poll_timer.data = (unsigned long)curstate;
	add_timer(&curstate->c_poll_timer);
	
	/* Create the character devices */

	tmpname = kmalloc(sizeof(char)*LJ_NAMESIZE, GFP_KERNEL);
	sprintf(tmpname, "lab%dportA",devid);
	curstate->achr_device.name = tmpname;
	curstate->achr_device.minor = minor; /* a's minor is start minor*/
	curstate->achr_device.fops = &achr_ops;
	

	result = misc_register(&curstate->achr_device);
  
	if(result){
		printk( KERN_INFO "Could not register porta.\n");
		goto err_intf;
	}
	else{
		printk( KERN_INFO "Registered a porta char dev!\n");
	}



	tmpname = kmalloc(sizeof(char)*LJ_NAMESIZE, GFP_KERNEL);
	sprintf(tmpname, "lab%dportB",devid);
	curstate->bchr_device.name = tmpname;
	curstate->bchr_device.minor = minor + 1; /* b's minor is start
						  * minor + 1 */
	curstate->bchr_device.fops = &bchr_ops;


	result = misc_register(&curstate->bchr_device);
  
	if(result){
		printk( KERN_INFO "Could not register portb.\n");
		goto err_rega;
	}
	else{
		printk( KERN_INFO "Registered a portb char dev!\n");
	}


	tmpname = kmalloc(sizeof(char)*LJ_NAMESIZE, GFP_KERNEL);
	sprintf(tmpname, "lab%dportC",devid);
	curstate->cchr_device.name = tmpname;
	curstate->cchr_device.minor = minor + 2; /* c's minor is start
						  * minor + 1 */
	curstate->cchr_device.fops = &cchr_ops;


	result = misc_register(&curstate->cchr_device);
  
	if(result){
		printk( KERN_INFO "Could not register portc.\n");
		goto err_regb;
	}
	else{
		printk( KERN_INFO "Registered a portc char dev!\n");
	}



	return 0;
  
err_regb:
	misc_deregister(&curstate->bchr_device);
	kfree(curstate->bchr_device.name);
err_rega:
	misc_deregister(&curstate->achr_device);
	kfree(curstate->achr_device.name);
err_intf:
	remove_state_table(minor);
err_hwlock:
	mutex_destroy(curstate->hw_lock);
	kfree(curstate->hw_lock);
err_free:
	usb_set_intfdata(intf, NULL);
	kfree(curstate);
  
error:
	return -1;
}

static  void lj_disconnect(struct usb_interface *intf)
{
	struct lj_state *curstate;
  
	int minor;
  
	printk(KERN_INFO "ByeBye HW!!!\n");
  
	curstate = usb_get_intfdata(intf);
  
	/* let the portC read syscall know there was an error */
	curstate->airlock = air_error;
	wake_up_interruptible(&curstate->c_waitqueue);
	
	del_timer_sync(&curstate->c_poll_timer);
	minor = curstate->bchr_device.minor;
	remove_state_table(minor);
  

	misc_deregister(&curstate->achr_device);
	kfree(curstate->achr_device.name);
  
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
	if (!lj_state){
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
	if(size < sizeof (int)){
		return -EINVAL;
	}
  
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
		printk(KERN_INFO "Could not successfully send bulk message\n");
		goto error;
	}
	else{
		printk("Successfully sent bulk message!\n");
	}


	result = usb_bulk_msg(lj_state->usb_device, 
			usb_rcvbulkpipe(lj_state->usb_device, 2),
			rcv_packet, RCVSIZE, &sent_len, 5);
  
	if(result){
		printk(KERN_INFO 
			"Could not successfully receive bulk message!\n");
		goto error;
	}
	else{
		printk("Successfully received bulk message!\n"
			"It was %d bytes. \n", sent_len);
	}

	if(rcv_packet[6]){
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


static ssize_t achr_read(struct file *file, char __user *buf, 
			size_t size, loff_t *off)
{
	printk(KERN_INFO "Someone tried to read on portA!\n");
	return -EINVAL;
}


static ssize_t cchr_read(struct file *file, char __user *buf, 
			size_t size, loff_t *off)
{
	struct lj_state *curstate;
	int cpysize;
	char *mesg = "Airlock open!";
	const int MESG_LEN = 14;

	printk(KERN_INFO "Someone tried to read on portC!\n");
	
	cpysize = (size < MESG_LEN) ? size : MESG_LEN;
	curstate = (struct lj_state*)file->private_data;
	if(wait_event_interruptible(curstate->c_waitqueue, 
					curstate->airlock != air_closed)){
		printk(KERN_INFO "error in cchr wait event!\n");
		return -ERESTARTSYS;
	}
	if(curstate->airlock == air_error)
	{
		return -ERESTARTSYS;
	}
	printk(KERN_INFO "cchar_read woke up!\n");
	copy_to_user(buf, mesg, cpysize);

	return cpysize;
}







static int __init lj_start(void)
{
	int result = 0;
	
	printk(KERN_INFO "Hello, kernel!\n");
	mutex_init(&state_table_lock);
	lj_state_table = kzalloc(sizeof(struct usb_interface*) * MAXDEV, 
				GFP_KERNEL);
  
	if(!lj_state_table){
		printk(KERN_INFO 
			"Could not allocate memory for interface table!\n");
		goto error;
	}


	result =  usb_register(&usb_driver);
	if (result){
		printk(KERN_INFO "Could not register device: %d", result);
		goto error_reg;
	}
	
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

