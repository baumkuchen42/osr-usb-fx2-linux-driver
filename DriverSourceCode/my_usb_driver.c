/************************************************
 * USB driver for the OSR FX2 board             *
 * Nick Mikstas                                 *
 * Based on usb-skeleton.c and osrfx2.c         *
 ************************************************/

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>

// infos that tell the usb subsystem, which calls to redirect to this driver
#define VENDOR_ID     0x0547       
#define PRODUCT_ID    0x1002
// minor number base to start minor numbers from
// which are then used to identify devices from this driver in the usb subsystem
#define MINOR_BASE    192

/*********************OSR FX2 vendor commands************************/
#define READ_LEDS     0xD7
#define SET_LEDS      0xD8
#define IS_HIGH_SPEED 0xD9

/**********************Function prototypes***************************/
// defining the function prototypes at the beginning of the file
// enables us to write the functions in a logical order
static int osrfx2_open(struct inode * inode, struct file * file);
static int osrfx2_release(struct inode * inode, struct file * file);
static ssize_t osrfx2_read(struct file * file, char * buffer, size_t count, loff_t * ppos);
static ssize_t osrfx2_write(struct file * file, const char * user_buffer, size_t count, loff_t * ppos);
static int osrfx2_probe(struct usb_interface * interface, const struct usb_device_id * id);
static void osrfx2_disconnect(struct usb_interface * interface);
static int osrfx2_suspend(struct usb_interface * intf, pm_message_t message);
static int osrfx2_resume(struct usb_interface * intf);
static void osrfx2_delete(struct kref * kref);
static void write_bulk_callback(struct urb *urb);
static ssize_t get_bargraph(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t set_bargraph(struct device * dev, struct device_attribute *attr, const char *buf,size_t count);

/***********************Module structures****************************/
/*Table of devices that work with this driver*/
// transforms vendor and product id from defines at top of this file to
// a subsystem-compatible format, the structure usb_device_id
static const struct usb_device_id osrfx2_id_table [] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { },
};

// makro to set it as the device table of this kernel module
MODULE_DEVICE_TABLE(usb, osrfx2_id_table);

/*OSR FX2 private device context structure*/
struct osrfx2 {    
    struct usb_device    * udev;        /* the usb device for this device */
    struct usb_interface * interface;       /* the interface for this device */    
    
    wait_queue_head_t FieldEventQueue;      /*Queue for poll and irq methods*/    
        
    unsigned char * bulk_in_buffer;     /*Transfer Buffers*/
    unsigned char * bulk_out_buffer;        
    
    size_t bulk_in_size;            /*Buffer sizes*/
    size_t bulk_out_size;
    
    __u8  bulk_in_endpointAddr;         /*USB endpoints*/
    __u8  bulk_out_endpointAddr;
    
    __u8  bulk_in_endpointInterval;     /*Endpoint intervals*/
    __u8  bulk_out_endpointInterval;
    
    struct urb * bulk_in_urb;           /*URBs*/
    struct urb * bulk_out_urb;
    
    struct kref kref;               /*Reference counter*/

    unsigned char leds;             /*LEDs status*/

    atomic_t bulk_write_available;      /*Track usage of the bulk pipes*/
    atomic_t bulk_read_available;

    size_t pending_data;            /*Data tracking for read write*/

    int suspended;                  /*boolean*/

    struct semaphore sem;           /*used during suspending and resuming device*/
    struct mutex io_mutex;          /*used during cleanup after disconnect*/
};

// basically the API of this driver, all the functions it provides for userspace applications
static const struct file_operations osrfx2_fops = {
    .owner   = THIS_MODULE,
    .open    = osrfx2_open,
    .release = osrfx2_release,
    .read    = osrfx2_read,
    .write   = osrfx2_write,
};

// the main usb driver structure which contains its usb methods and id table (and of course the name)
static struct usb_driver osrfx2_driver = {
    .name        = "osrfx2",
    .probe       = osrfx2_probe,
    .disconnect  = osrfx2_disconnect,
    .suspend     = osrfx2_suspend,
    .resume      = osrfx2_resume,
    .id_table    = osrfx2_id_table,
};

/*Used to get a minor number from the usb core
  and register device with devfs and driver core*/
static struct usb_class_driver osrfx2_class = {
    .name       = "device/osrfx2_%d",
    .fops       = &osrfx2_fops,
    .minor_base = MINOR_BASE,
};

/***********************Module functions*****************************/
/*Create device attribute bargraph*/
// arguments: name, mode, getter, setter
static DEVICE_ATTR(bargraph, 0660, get_bargraph, set_bargraph);

/*insmod*/
// the driver's init function called on insmod
int init_module(void) {
    int retval;

    retval = usb_register(&osrfx2_driver);

	// if the return value from registering is not 0, we know we got an error
    if(retval != 0)
        pr_err("usb_register failed. Error number %d", retval);

    return retval;
}

/*rmmod*/
// the driver's exit function called on rmmod
void cleanup_module(void) {
    usb_deregister(&osrfx2_driver);
}

// the probe function that the usb subsystem calls when the usb controller registers a new usb device
// that matches the usb vendor id and product id associated with this driver
// it initialises the device and endpoints 
static int osrfx2_probe(struct usb_interface * intf, const struct usb_device_id * id) {
	// takes the interface it got passed from the usb subsystem and makes it a usb device
	// which can then be used for some function calls to the usb core which only take devices, not interfaces
    struct usb_device *udev = interface_to_usbdev(intf);
    struct osrfx2 *fx2dev = NULL; // create a new osrfx2 structure to contain all necessary info for this driver
    struct usb_endpoint_descriptor *endpoint; // also create an endpoint structure for later
    int retval, i; // create return value variable for errors

    /*Create and initialize context struct*/
    fx2dev = kmalloc(sizeof(struct osrfx2), GFP_KERNEL); // allocates space for main driver information structure
    if (fx2dev == NULL) { // if no space allocated
        retval = -ENOMEM; // memory error
        dev_err(&intf->dev, "OSR USB-FX2 device probe failed: %d.\n", retval);
        if (fx2dev != 0) { // if error code returned from allocation
        	kref_put(&fx2dev->kref, osrfx2_delete); // decrements usage count and free ressources 
        }
        return retval;
    }

    /*Zero out fx2dev struct*/
    memset(fx2dev, 0, sizeof(*fx2dev)); // fills the allocated space with zeros to initialise it

    /*Set initial fx2dev struct members*/
    kref_init(&fx2dev->kref); // initialises reference counter
    mutex_init(&fx2dev->io_mutex); // initialises mutex
    sema_init(&fx2dev->sem, 1); // initialises semaphore
    init_waitqueue_head(&fx2dev->FieldEventQueue); // initialises waiting queue
    
    fx2dev->udev = usb_get_dev(udev); // sets udev attr to udev extracted from interface before
    fx2dev->interface = intf;
    
    // sets the bit that indicates if writing is possible,
    // has to be thread-safe because multiple applications might query it at once
    fx2dev->bulk_write_available = (atomic_t) ATOMIC_INIT(1);
    fx2dev->bulk_read_available  = (atomic_t) ATOMIC_INIT(1);
    
    // saves a pointer to the USB driver specific interface,
    // instead of having to keep a static array of device pointers for every driver
    usb_set_intfdata(intf, fx2dev);

    /*create sysfs attribute files for device components.*/
    // creates file for bargraph attribute in the device directory so applications can access it
    retval = device_create_file(&intf->dev, &dev_attr_bargraph);
    if (retval != 0) {
        dev_err(&intf->dev, "OSR FX2 device probe failed: %d.\n", retval);
        // if the context struct is now faulty, decrement usage count and clean up
        if (fx2dev != 0) {
        	kref_put(&fx2dev->kref, osrfx2_delete);
        }
        return retval;
    }

    /*Set up the endpoint information*/
    // set up as many endpoints as specified in the current altsetting description of the interface
    for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
    	// type and other attributes of endpoint are specified in description in endpoint list
        endpoint = &intf->cur_altsetting->endpoint[i].desc;

		// uses usb subsystem method to find out type of endpoint,
		// then creates endpoint in context struct with given attributes from interface
        if(usb_endpoint_is_bulk_in(endpoint)) { /*Bulk in*/
            fx2dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
            fx2dev->bulk_in_endpointInterval = endpoint->bInterval;
            fx2dev->bulk_in_size = endpoint->wMaxPacketSize;
        }
        if(usb_endpoint_is_bulk_out(endpoint)) { /*Bulk out*/
            fx2dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
            fx2dev->bulk_out_endpointInterval = endpoint->bInterval;
            fx2dev->bulk_out_size = endpoint->wMaxPacketSize;
        }
    }
    /*Error if incorrect number of endpoints found*/
    // basically if no endpoints were created at all
    if (
    	fx2dev->bulk_in_endpointAddr  == 0 ||
        fx2dev->bulk_out_endpointAddr == 0
    ) {
        retval = -ENODEV;
        dev_err(&intf->dev, "OSR FX2 device probe failed: %d\n", retval);
        // if device context structure is faulty now, decrement usage count and clean up
        if (fx2dev != 0) {
        	kref_put(&fx2dev->kref, osrfx2_delete);
        }
        return retval;
    }

    /*Initialize bulk endpoint buffers*/
    // first allocate space in kernel space
    fx2dev->bulk_in_buffer = kmalloc(fx2dev->bulk_in_size, GFP_KERNEL);
    // if allocated space is NULL, go into error routine
    if (!fx2dev->bulk_in_buffer) {
        retval = -ENOMEM;
        dev_err(&intf->dev, "OSR FX2 device probe failed: %d.\n", retval);
        // if device context structure is faulty now, clean up
        if (fx2dev != 0) {
        	kref_put(&fx2dev->kref, osrfx2_delete);
        }
        return retval;
    }
    
    // first allocate space in kernel space
    fx2dev->bulk_out_buffer = kmalloc(fx2dev->bulk_out_size, GFP_KERNEL);
    // if allocated space is NULL, go into error routine
    if (!fx2dev->bulk_out_buffer) {
        retval = -ENOMEM;
        dev_err(&intf->dev, "OSR FX2 device probe failed: %d.\n", retval);
        // if device context structure is faulty now, decrement usage count and clean up
        if (fx2dev != 0) {
        	kref_put(&fx2dev->kref, osrfx2_delete);
        }
        return retval;
    }

    /*Register device*/
    // finally, after all attributes are set and/or initialised, register the usb device
    // at the usb subsystem with the interface given by poll and the class defined at the
    // top of this file
    retval = usb_register_dev(intf, &osrfx2_class);
    // if the registering didn't work, this particular interface is invalid, so set to NULL
    if (retval != 0) {
        usb_set_intfdata(intf, NULL);
    }

    dev_info(&intf->dev, "OSR FX2 device now attached\n");

    return 0;
}

// this method gets called when the board is disconnected from the computer
// the usb subsystem calls the disconnect method of its driver
// which then cleans up all references and ressources allocated for this
// device instance
static void osrfx2_disconnect(struct usb_interface * intf) {
    struct osrfx2 * fx2dev;

	// get saved data from interface back and save it in context struct
    fx2dev = usb_get_intfdata(intf);
    // clear interface by setting data to NULL
    usb_set_intfdata(intf, NULL);

    /*Give back minor*/
    usb_deregister_dev(intf, &osrfx2_class);

    /*Prevent more I/O from starting*/
    mutex_lock(&fx2dev->io_mutex);
    // also set interface attribute of context struct to NULL
    fx2dev->interface = NULL;
    mutex_unlock(&fx2dev->io_mutex);

    /*Remove sysfs files*/
    device_remove_file(&intf->dev, &dev_attr_bargraph);

    /*Decrement usage count*/
    // and free ressources
    kref_put(&fx2dev->kref, osrfx2_delete);

    dev_info(&intf->dev, "OSR FX2 disconnected.\n");
}

/*Delete resources used by this device*/
static void osrfx2_delete(struct kref * kref) {
	// get the context structure that contains the reference
    struct osrfx2 *fx2dev = container_of(kref, struct osrfx2, kref);

	// releases the use of the usb device structure
    usb_put_dev(fx2dev->udev);
    
    // frees memory allocated for bulk in buffer
    if (fx2dev->bulk_in_buffer) {
        kfree(fx2dev->bulk_in_buffer);
    }
    // frees memory allocated for bulk out buffer
    if (fx2dev->bulk_out_buffer) {
        kfree(fx2dev->bulk_out_buffer);
	}
	// frees memory allocated for device context structure, thus deleting it
    kfree(fx2dev);
}

/*Suspend device*/
// when the kernel thinks the device is not doing anything important and it
// wants to save energy, it sends a suspend command to the device, then
// this method is called
static int osrfx2_suspend(struct usb_interface * intf, pm_message_t message) {
    struct osrfx2 * fx2dev = usb_get_intfdata(intf);

	// tries to acquire the semaphore, error if not successful
	// sleeps if no more tasks are allowed to acquire the semaphore
	// sleep can be interrupted by interrupt
	// this means that suspending is really only possible no other
	// method is accessing the context structure so the device
	// is really not busy
    if (down_interruptible(&fx2dev->sem) != 0) {
        return -ERESTARTSYS;
    }    

	// set suspended flag in context structure
    fx2dev->suspended = 1;

	// release the semaphore again
    up(&fx2dev->sem);

    return 0;
}

/*Wake up device*/
// gets called when device is resumed again by power management
static int osrfx2_resume(struct usb_interface * intf) {
    // get context structure from interface where it was saved as data
    struct osrfx2 * fx2dev = usb_get_intfdata(intf);

	// tries to get access to context structure
    if (down_interruptible(&fx2dev->sem) != 0) {
        return -ERESTARTSYS;
    }
    
    // set suspended flag in context
    fx2dev->suspended = 0;
    
    // release the semaphore again
    up(&fx2dev->sem);

    return 0;
}

/*Open device for reading and writing*/
static int osrfx2_open(struct inode * inode, struct file * file) {
    struct usb_interface *interface;
    struct osrfx2        *fx2dev;
    int retval;
    int flags;
    
    // get interface from inode, first minor number from inode
    interface = usb_find_interface(&osrfx2_driver, iminor(inode));
    // if interface extraction failed, return no device error
    if (!interface) {
    	return -ENODEV;
    }

	// get context structure from data saved in interface
    fx2dev = usb_get_intfdata(interface);
    // return no device error if context structure extraction failed
    if (!fx2dev) {
    	return -ENODEV;
    }

    /*Serialize access to each of the bulk pipes*/
    // get access mode flags passed with open call
    flags = (file->f_flags & O_ACCMODE);

	// if device was opened for write
    if ((flags == O_WRONLY) || (flags == O_RDWR)) {
    	// check if writing is available by decrementing available bit
        if (!atomic_dec_and_test(&fx2dev->bulk_write_available)) {
        	// if write is not available, count up again and return busy error
            atomic_inc(&fx2dev->bulk_write_available);
            return -EBUSY;
        }

        /*The write interface is serialized, so reset bulk-out pipe (ep-6)*/
        // clear halt clears endpoint of given device
        retval = usb_clear_halt(fx2dev->udev, fx2dev->bulk_out_endpointAddr);
        if ((retval != 0) && (retval != -EPIPE)) {
            dev_err(&interface->dev, "%s - error(%d) usb_clear_halt(%02X)\n",
                    __FUNCTION__, retval, fx2dev->bulk_out_endpointAddr);
        }
    }

	// if device was opened for read, routine analogous to write initialising above
    if ((flags == O_RDONLY) || (flags == O_RDWR)) {
        if (!atomic_dec_and_test(&fx2dev->bulk_read_available)) {
            atomic_inc(&fx2dev->bulk_read_available);
            if (flags == O_RDWR) {
            	// also reset write available if openend for reading and writing
                atomic_inc(&fx2dev->bulk_write_available);
            }
            return -EBUSY;
        }

        /*The read interface is serialized, so reset bulk-in pipe (ep-8)*/
        retval = usb_clear_halt(fx2dev->udev, fx2dev->bulk_in_endpointAddr);
        if ((retval != 0) && (retval != -EPIPE)) {
            dev_err(&interface->dev, "%s - error(%d) usb_clear_halt(%02X)\n",
                    __FUNCTION__, retval, fx2dev->bulk_in_endpointAddr);
        }
    }

    /*Set this device as non-seekable*/
    retval = nonseekable_open(inode, file);
    if (retval != 0) {
    	return retval;
    }

    /*Increment our usage count for the device*/
    kref_get(&fx2dev->kref);

    /*Save pointer to device instance in the file's private structure*/
    file->private_data = fx2dev;

    return 0;
}

/*Release device*/
// gets called when user application closes the file again
static int osrfx2_release(struct inode * inode, struct file * file) {
    struct osrfx2 * fx2dev;
    int flags;

	// get context structure from data saved in file structure on open
    fx2dev = (struct osrfx2 *)file->private_data;
    // return no device error if fetching didn't work
    if (!fx2dev) {
        return -ENODEV;
    }

    /*Release any bulk_[write|read]_available serialization*/
    flags = (file->f_flags & O_ACCMODE);

    if ((flags == O_WRONLY) || (flags == O_RDWR)) {
        atomic_inc(&fx2dev->bulk_write_available);
    }

    if ((flags == O_RDONLY) || (flags == O_RDWR)) {
        atomic_inc(&fx2dev->bulk_read_available);
    }
 
    /*Decrement the ref-count on the device instance*/
    kref_put(&fx2dev->kref, osrfx2_delete);

    return 0;
}

/*Read from /dev/osrfx2_0*/
static ssize_t osrfx2_read(struct file * file, char * buffer, size_t count, loff_t * ppos) {
    struct osrfx2 *fx2dev;
    int retval = 0;
    int bytes_read;
    int pipe;

	// get context structure from data saved in file structure on open
    fx2dev = (struct osrfx2 *)file->private_data;

    /*Initialize pipe*/
    // create pipe/endpoint for reading at endpoint address specified in context structure
    pipe = usb_rcvbulkpipe(fx2dev->udev, fx2dev->bulk_in_endpointAddr),

    /*Do a blocking bulk read to get data from the device*/
    // data is then saved in bulk in buffer
    retval = usb_bulk_msg(fx2dev->udev, pipe, fx2dev->bulk_in_buffer, min(fx2dev->bulk_in_size, count),
                          &bytes_read, 10000);

    /*If the read was successful, copy the data to userspace */
    if (retval == 0) {
        if (copy_to_user(buffer, fx2dev->bulk_in_buffer, bytes_read))
            retval = -EFAULT;
        else
            retval = bytes_read;        
        
        /*Increment the pending_data counter by the byte count received*/
        fx2dev->pending_data -= retval;
    }

    return retval;
}

/*Write to bulk endpoint*/
static ssize_t osrfx2_write(struct file * file, const char * user_buffer, size_t count, loff_t * ppos) {
    struct osrfx2 *fx2dev;
    struct urb *urb = NULL;
    char *buf = NULL;
    int pipe;
    int retval = 0;

    fx2dev = (struct osrfx2 *)file->private_data;

	// if nothing is supposed to be written, just return 0 instead of proceeding
    if (count == 0) {
    	return count;
   	}
 
    /*Create a urb*/
    urb = usb_alloc_urb(0, GFP_KERNEL);

	// if urb creation failed
    if(!urb) {
        retval = -ENOMEM;
        // free space allocated with usb_alloc_coherent
        usb_free_coherent(fx2dev->udev, count, buf, urb->transfer_dma);
        usb_free_urb(urb);
        return retval;
    }

    /*Create urb buffer*/
    buf = usb_alloc_coherent(fx2dev->udev, count, GFP_KERNEL, &urb->transfer_dma);

	// if buffer allocation failed
    if(!buf) {
        retval = -ENOMEM;
        usb_free_coherent(fx2dev->udev, count, buf, urb->transfer_dma);
        usb_free_urb(urb);
        return retval;
    }

    /*Copy the data to the buffer*/
    if(copy_from_user(buf, user_buffer, count) != 0) {
    	// return general fault and free allocated space and urb if copying failed
        retval = -EFAULT;
        usb_free_coherent(fx2dev->udev, count, buf, urb->transfer_dma);
        usb_free_urb(urb);
        return retval;
    }

    /*Initialize the urb*/
    // initialise a pipe/endpoint for sending at out endpoint address
    pipe = usb_sndbulkpipe(fx2dev->udev, fx2dev->bulk_out_endpointAddr);
    // fill allocated urb with context data and read data from user
    usb_fill_bulk_urb(urb, fx2dev->udev, pipe, buf, count, write_bulk_callback, fx2dev);
    // signifies that driver provides this DMA address, 
    // which the host controller driver should use in preference to the transfer_buffer
    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /*Send the data out the bulk port*/
    retval = usb_submit_urb(urb, GFP_KERNEL);

	// if submitting the urb failed, free allocated space and urb and return error
    if (retval != 0) {
        dev_err(&fx2dev->interface->dev, "%s - usb_submit_urb failed: %d\n", __FUNCTION__, retval);
        usb_free_coherent(fx2dev->udev, count, buf, urb->transfer_dma);
        usb_free_urb(urb);
        return retval;
    }

    /*Increment the pending_data counter by the byte count sent*/
    fx2dev->pending_data += count;
     
    /*Release the reference to this urb*/
    usb_free_urb(urb);

    return count;
}

// this method gets called when writing is finished
static void write_bulk_callback(struct urb * urb) {
    struct osrfx2 *fx2dev = (struct osrfx2 *)urb->context;
 
    /*  Filter sync and async unlink events as non-errors*/
    // if an error occured which is not one of those three ones, log the error
    if(urb->status != 0 && !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)) {
        dev_err(&fx2dev->interface->dev, "%s - non-zero status received: %d\n", __FUNCTION__, urb->status);
    }
 
    /*Free the spent buffer*/
    usb_free_coherent( urb->dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma );
}

/*Gets the LED bargraph status on the device*/
// method that gets called when device attribute bargraph is queried
static ssize_t get_bargraph(struct device *dev, struct device_attribute *attr, char *buf) {
    struct usb_interface  *intf   = to_usb_interface(dev);
    struct osrfx2         *fx2dev = usb_get_intfdata(intf);
    int retval;
   
    // if device is suspended, write S to buffer that was passed to
    // write device status into and abort
    if (fx2dev->suspended) {
        return sprintf(buf, "S ");   /*Device is suspended*/
    }

	// clear leds attribute first
    fx2dev->leds = 0;

    /*Get LED values*/
    // send request as control message via usb subsystem
    retval = usb_control_msg(
    	fx2dev->udev, // device
    	usb_rcvctrlpipe(fx2dev->udev, 0), // pipe specifying device address
        READ_LEDS, // request message
        USB_DIR_IN | USB_TYPE_VENDOR, // request type: read & usb identifier
        0, // value
        0, // index
        &fx2dev->leds, // attribute where results should be written to
        sizeof(fx2dev->leds), // size of result variable
        USB_CTRL_GET_TIMEOUT // timeout
    );

    /*Fill buffer with LED status*/
    // sprintf creates a new string representing the values of the LED array
    retval = sprintf(
    	buf, "%s%s%s%s%s%s%s%s",
    	// bitwise AND of specific index in LED bit array and the array
    	// to check if value is 1 there, if yes, set to 1 in result string
        (fx2dev->leds & 0x10) ? "1" : "0", // 0001 0000
        (fx2dev->leds & 0x08) ? "1" : "0", // 0000 1000
        (fx2dev->leds & 0x04) ? "1" : "0", // 0000 0100
        (fx2dev->leds & 0x02) ? "1" : "0", // 0000 0010
        (fx2dev->leds & 0x01) ? "1" : "0", // 0000 0001
        (fx2dev->leds & 0x80) ? "1" : "0", // 1000 0000
        (fx2dev->leds & 0x40) ? "1" : "0", // 0100 0000
        (fx2dev->leds & 0x20) ? "1" : "0" // 0010 0000
    );

    return retval;
}

/*Sets the LED bargraph on the device*/
// method that gets called when device attribute bargraph is set
static ssize_t set_bargraph(struct device * dev, struct device_attribute *attr, const char *buf,size_t count) {
    struct usb_interface  *intf   = to_usb_interface(dev);
    struct osrfx2         *fx2dev = usb_get_intfdata(intf);

    unsigned int value;
    int retval;
    char *end;

	// first clear leds attribute in context structure
    fx2dev->leds = 0;

    /*convert buffer to unsigned long*/
    // buf is the start of the string, end the end and 10 the number base
    // result is then initialised with all 1
    value = (simple_strtoul(buf, &end, 10) & 0xFF);
    // if the buffer length is 0, set value to zero
    if (buf == end) {
        value = 0;
    }

    /*Check range of value 0 =< value < 256*/   
    // avoid overflow 
    if(value > 255) {
        fx2dev->leds = 0;
    }
    else { /*convert to intuitive bit system. bit 0 = bottom, bit 7 = top*/
    	// bitwise OR of leds with value extracted from function param at specific position
    	// shift value three to the right and then AND with first position
        fx2dev->leds |= ((value >> 3) & 0x01); // 8765 4321 -> 0008 7654, selecting the 4th position
        fx2dev->leds |= ((value >> 3) & 0x02); // 8765 4321 -> 0008 7654, selecting the 5th position
        fx2dev->leds |= ((value >> 3) & 0x04); // 8765 4321 -> 0008 7654, selecting the 6th position
        fx2dev->leds |= ((value >> 3) & 0x08); // 8765 4321 -> 0008 7654, selecting the 7th position
        fx2dev->leds |= ((value >> 3) & 0x10); // 8765 4321 -> 0008 7654, selecting the 8th posisition
        // shift value 5 to the left
        fx2dev->leds |= ((value << 5) & 0x20); // 8765 4321 -> 3210 0000, selecting the 1st position
        fx2dev->leds |= ((value << 5) & 0x40); // 8765 4321 -> 3210 0000, selecting the 2nd position
        fx2dev->leds |= ((value << 5) & 0x80); // 8765 4321 -> 3210 0000, selecting the 3rd position
    }

    /*Set LED values*/
    retval = usb_control_msg(
    	fx2dev->udev, // device
    	usb_sndctrlpipe(fx2dev->udev, 0), // pipe specifying device address
        SET_LEDS, // request message
        USB_DIR_OUT | USB_TYPE_VENDOR, // request type: write & usb identifier
        0, // value
        0, // index
        &fx2dev->leds, // attribute where results should be written to
        sizeof(fx2dev->leds), // size of result variable
        USB_CTRL_GET_TIMEOUT // timeout
    );

	// log error
    if (retval < 0) {
        dev_err(&fx2dev->udev->dev, "%s - retval=%d\n", __FUNCTION__, retval);
    }

	// return how many bytes where written
    return count;
}

MODULE_DESCRIPTION("OSR FX2 Linux Driver");
MODULE_AUTHOR("Nick Mikstas");
MODULE_LICENSE("GPL");
