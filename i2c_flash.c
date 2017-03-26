#include <linux/module.h>  // Module Defines and Macros (THIS_MODULE)
#include <linux/kernel.h>  // 
#include <linux/fs.h>	   // Inode and File types
#include <linux/cdev.h>    // Character Device Types and functions.
#include <linux/types.h>
#include <linux/slab.h>	   // Kmalloc/Kfree
#include <asm/uaccess.h>   // Copy to/from user space
#include <linux/string.h>
#include <linux/device.h>  // Device Creation / Destruction functions
#include <linux/i2c.h>     // i2c Kernel Interfaces
#include <linux/i2c-dev.h>
#include <linux/ioctl.h>
#include <linux/string.h> // for memcpy
#include <linux/delay.h>  // for msleep
#include <linux/errno.h>       
#include <linux/gpio.h>
#include <asm/gpio.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>  
#include<linux/init.h>
#include<linux/moduleparam.h> // Passing parameters to modules through insmod

#define DEVICE_NAME "i2c_flash"  // device name to be created and registered
#define DEVICE_ADDR 0x54        //device address A2 pin high
#define I2CMUX 29 
#define BUSY_LED 26

#define MY_IOCTL 'G'
#define FLASHGETS _IOR(MY_IOCTL, 0, int)     // busy status is read 
#define FLASHGETP _IOR(MY_IOCTL, 1, int)   // gets current address 
#define FLASHSETP _IOW(MY_IOCTL, 2, int)   // sets address
#define FLASHERASE _IO(MY_IOCTL, 3)          // erases memory
	
DEFINE_MUTEX(my_mutex);             
/* per device structure */
struct i2c_dev {
	struct cdev cdev;               /* The cdev structure */
	// Local variables
	struct i2c_client *client;
	struct i2c_adapter *adapter;
    short current_page;                // current address(page) global var
	int count;
    bool busy_flag;
    char* data_buf;
	bool data_available;
} *i2c_devp;

static dev_t i2c_dev_number;          /* Allotted device number    */
struct class *i2c_dev_class;          /* Tie with the device model */
static struct device *i2c_dev_device;

static struct workqueue_struct *my_wq;

typedef struct {
  struct work_struct my_work;
  struct i2c_dev *i2c_devpw;
} my_work_t;

my_work_t *work;

static void write_wq_fn(struct work_struct* work)
{
	my_work_t *my_work = (my_work_t*)work;
    struct i2c_dev *i2c_devp = my_work->i2c_devpw;
	int cpage =  i2c_devp->current_page;
	short add = cpage*64;                          // add is 16 bit mem address 
	char send_data[66];                    
	int count = i2c_devp->count;
	int ret;
	int page;

	gpio_set_value_cansleep(BUSY_LED, 1);
	for(page=0;page<count;page++)
	{		
        memcpy(send_data,((char*)&add)+1,1);                             
		memcpy(&(send_data[1]),(char*)&add,1);
        memcpy(&(send_data[2]),(i2c_devp->data_buf +(page*64)),64);
		
		// write 64 bytes 
		ret = i2c_master_send(i2c_devp->client,send_data,66);
		if(ret < 0)
		{
			gpio_set_value_cansleep(BUSY_LED, 0);
			printk("Error: could not write or busy.\n");
			return;
		}
	    add=add+64;
        cpage++;

        if(add == (512*64))
			add = 0;         // to wrap around
		if(cpage == 512)
			cpage = 0;       // to wrap around 
		msleep(5);		
	}
	gpio_set_value_cansleep(BUSY_LED, 0);
    i2c_devp->current_page=cpage;
	kfree(i2c_devp->data_buf);
	i2c_devp->busy_flag = 0;
	kfree(work);
		
}

static void read_wq_fn(struct work_struct* work)
{
	my_work_t *my_work = (my_work_t*)work;
    struct i2c_dev *i2c_devp = my_work->i2c_devpw;
	int cpage = i2c_devp->current_page; 
	int count = i2c_devp->count;
	short add = cpage*64;                               // add is 16 bit mem address 
	char send_add[2];                                       
	int ret;
    i2c_devp->data_buf = kmalloc(sizeof(char)*count*64, GFP_KERNEL);
	if(i2c_devp->data_buf < 0)
		{
			printk("could not allocate memory");
			return;
		}
    msleep(10);
    memcpy(send_add,((char*)&add)+1,1);
	memcpy(&(send_add[1]),((char*)&add),1);
	gpio_set_value_cansleep(BUSY_LED, 1);
	ret = i2c_master_send(i2c_devp->client,(void*)&send_add, 2);  // page address set ,first write 1 word to set page address, since only first 2 bytes is sent no data is written 
	if(ret < 0)
	{
		printk("Error: could not send page addr.\n");
		return;
	}
	ret = i2c_master_recv(i2c_devp->client, (void*)i2c_devp->data_buf, count*64);
	if(ret < 0)
	{
		printk("Error: could not read .\n");
		return;
	}        

	cpage = (cpage + count)%512;
	gpio_set_value_cansleep(BUSY_LED, 0);
    i2c_devp->current_page = cpage;
    i2c_devp->data_available = 1;       // ready for copy to user now
	i2c_devp->busy_flag = 0;            // not busy
	kfree(work);
}
/*
* Open driver
*/
int i2c_driver_open(struct inode *inode, struct file *file)
{
	struct i2c_dev *i2c_devp;

	/* Get the per-device structure that contains this cdev */
	i2c_devp = container_of(inode->i_cdev, struct i2c_dev, cdev);

	/* Easy access to i2c_devp from rest of the entry points */
	file->private_data = i2c_devp;
	printk("i2c Device is opening.\n");
	return 0;
}

/*
 * Release driver
 */
int i2c_driver_release(struct inode *inode, struct file *file)
{
	//struct i2c_dev *i2c_devp = file->private_data;
	//printk("i2c Device is closing.\n");
	return 0;
}

/*
 * Write to driver
 */
ssize_t i2c_driver_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	struct i2c_dev *i2c_devp = file->private_data;
	int ret;
	
	if(i2c_devp->busy_flag == 1)
		return -EBUSY;
	else
	{
		my_work_t *work;
		i2c_devp->busy_flag = 1;  // 1 is busy
		i2c_devp->count = (int)count;
		i2c_devp->data_buf = kmalloc(sizeof(char)*count*64, GFP_KERNEL);
        if(i2c_devp->data_buf <0)
			return -1;
		ret =  copy_from_user(i2c_devp->data_buf,buf,count*64);
		if(ret < 0)
			return -EINVAL;
		work = (my_work_t*)kmalloc(sizeof(my_work_t), GFP_KERNEL);
		work->i2c_devpw = i2c_devp;
		INIT_WORK((struct work_struct*)work, write_wq_fn);
		ret = queue_work(my_wq,(struct work_struct*)work); 
        return 0;
	}
}

/*
 * Read from driver
 */
ssize_t i2c_driver_read(struct file *file, char *ubuf, size_t count, loff_t *ppos)  
{
	struct i2c_dev *i2c_devp = file->private_data;
	int ret;
	
	if(i2c_devp->busy_flag == 1)
			return -EBUSY;

	else if( i2c_devp->data_available == 1)
	{
		ret = copy_to_user(ubuf,i2c_devp->data_buf, 64*count);
		if(ret < 0)		
		return -EINVAL;
		kfree(i2c_devp->data_buf);
		i2c_devp->data_available = 0;
		return 0;
	}
	else
	{
		my_work_t *work;
		i2c_devp->busy_flag = 1;  // 1 is busy
		i2c_devp->count = (int)count;
		work = (my_work_t*)kmalloc(sizeof(my_work_t), GFP_KERNEL);
		if(work < 0 )
			return -1;
		work->i2c_devpw = i2c_devp;
		INIT_WORK((struct work_struct*)work, read_wq_fn);
		ret = queue_work(my_wq,(struct work_struct*)work);  
        return -EAGAIN;		
	}
}
  
static long i2c_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{   
	struct i2c_dev *i2c_devp = file->private_data;
    int cpage = i2c_devp->current_page;
	short add = cpage*64;
	int ret=0,page=0;
	char data_buf[64],send_data[66];	
	mutex_lock(&my_mutex);   //lock_kernel();
	switch (cmd)
	{
		case FLASHGETS:  printk("FLASHGETS recvd\n");
						 if(i2c_devp->busy_flag == 1)
							ret = 1;
						 else 
						 	ret = 0;		
						 break;
		case FLASHGETP:  printk("FLASHGETP recvd\n");
								ret = cpage;
						 break;
        case FLASHSETP:  printk("FLASHSETP recvd, set page :%d \n", (int)arg);
						 i2c_devp->current_page = arg;
						 ret =0;
						 break;
		case FLASHERASE: printk("FLASHERASE recvd \n");
	 					 memset(data_buf,'1',64);
			             add =0x0000;
						 gpio_set_value_cansleep(BUSY_LED, 1);
						 for(page=0;page<512;page++)
							{		
								memcpy(send_data,((char*)&add)+1,1);
								memcpy(&(send_data[1]),&add,1);
								memcpy(&(send_data[2]),data_buf,64);
		
								// write 64 bytes 
								ret = i2c_master_send(i2c_devp->client,(void*)send_data, 66);
								if(ret < 0)
								{
									gpio_set_value_cansleep(BUSY_LED, 0);
									printk("Error: could not erase or busy.\n");
									return -1;
								}
								add = add + 64; 						
								msleep(5);		
							}
						 gpio_set_value_cansleep(BUSY_LED, 0);
						 ret =0;
						 break;
	}
	mutex_unlock(&my_mutex);  //unlock_kernel();
	return ret;
}

/* File operations structure. Defined in linux/fs.h */
static struct file_operations i2c_fops = {
    .owner		= THIS_MODULE,           /* Owner */
    .open		= i2c_driver_open,        /* Open method */
    .release	= i2c_driver_release,     /* Release method */
    .write		= i2c_driver_write,       /* Write method */
    .read		= i2c_driver_read,        /* Read method */
	.unlocked_ioctl = i2c_ioctl,
};

/*
 * Driver Initialization
 */
int __init i2c_driver_init(void)
{
	int ret;
	
	/* Request dynamic allocation of a device major number */
	if (alloc_chrdev_region(&i2c_dev_number, 0, 1, DEVICE_NAME) < 0) {
			printk(KERN_DEBUG "Can't register device\n"); return -1;
	}

	/* Populate sysfs entries */
	i2c_dev_class = class_create(THIS_MODULE, DEVICE_NAME);

	/* Allocate memory for the per-device structure */
	i2c_devp = kmalloc(sizeof(struct i2c_dev), GFP_KERNEL);
		
	if (!i2c_devp) {
		printk("Bad Kmalloc\n"); return -ENOMEM;
	}

	/* Request I/O region */

	/* Connect the file operations with the cdev */
	cdev_init(&i2c_devp->cdev, &i2c_fops);
	i2c_devp->cdev.owner = THIS_MODULE;

	/* Connect the major/minor number to the cdev */
	ret = cdev_add(&i2c_devp->cdev, (i2c_dev_number), 1);

	if (ret) {
		printk("Bad cdev\n");
		return ret;
	}

	/* Send uevents to udev, so it'll create /dev nodes */
	i2c_dev_device = device_create(i2c_dev_class, NULL, MKDEV(MAJOR(i2c_dev_number), 0), NULL, DEVICE_NAME);	

	ret = gpio_request(I2CMUX, "I2CMUX");
	if(ret)
	{
		printk("GPIO %d is not requested.\n", I2CMUX);
	}

	ret = gpio_direction_output(I2CMUX, 0);
	if(ret)
	{
		printk("GPIO %d is not set as output.\n", I2CMUX);
	}
	gpio_set_value_cansleep(I2CMUX, 0); // Direction output didn't seem to init correctly.	

	ret = gpio_request(BUSY_LED, "BUSY_LED");
	if(ret)
	{
		printk("\n\t i2cflash GPIO %d is not requested.\n", BUSY_LED);
	}

	ret = gpio_direction_output(BUSY_LED, 0);
	if(ret)
	{
		printk("\n\t i2cflash GPIO %d is not set as output.\n", BUSY_LED);
	}
	gpio_set_value_cansleep(BUSY_LED, 0); // Direction output didn't seem to init correctly.	
	
	/* Create Work Queue */
	my_wq = create_workqueue("my_queue");

	// Create Adapter using:
	i2c_devp->adapter = i2c_get_adapter(0); // /dev/i2c-0
	if(i2c_devp->adapter == NULL)
	{
		printk("Could not acquire i2c adapter.\n");
		return -1;
	}

	// Create Client Structure
	i2c_devp->client = (struct i2c_client*) kmalloc(sizeof(struct i2c_client), GFP_KERNEL);
	i2c_devp->client->addr = DEVICE_ADDR; // Device Address (set by hardware)
	snprintf(i2c_devp->client->name, I2C_NAME_SIZE, "i2c_i2c102");
	i2c_devp->client->adapter = i2c_devp->adapter;
	
	// device structure initialisations
	i2c_devp->current_page = 0;
	i2c_devp->busy_flag = 0;
	i2c_devp->data_available = 0;
	i2c_devp->count = 0;
	
	return 0;
}
/* Driver Exit */
void __exit i2c_driver_exit(void)
{
	// destroy and clean work-queue
	 flush_workqueue( my_wq );
     destroy_workqueue( my_wq );

	// Close and cleanup
	i2c_put_adapter(i2c_devp->adapter);
	kfree(i2c_devp->client);

	/* Release the major number */
	unregister_chrdev_region((i2c_dev_number), 1);

	/* Destroy device */
	device_destroy (i2c_dev_class, MKDEV(MAJOR(i2c_dev_number), 0));
	cdev_del(&i2c_devp->cdev);
	kfree(i2c_devp);
	
	/* Destroy driver_class */
	class_destroy(i2c_dev_class);
	
	gpio_free(I2CMUX);
	gpio_free(BUSY_LED);
}

module_init(i2c_driver_init);
module_exit(i2c_driver_exit);
MODULE_LICENSE("GPL v2");

