/*
 * m4_udooneo_debugger.c - tty driver used to comunicate with
 * M4 core on posix tty interface.
 *
 * Copyright (C) 2015-2016 Seco S.r.L. All Rights Reserved.
 * Based on IMX MCC TEST driver.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

//====================================================
// new version fefr 040216
// Send messages moved in thread_function
//====================================================

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/mcc_config_linux.h>
#include <linux/mcc_common.h>
#include <linux/mcc_api.h>

#include<linux/init.h>
#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/kthread.h>
#include<linux/sched.h>

// used for send quee messages
#ifdef SEND_IS_INTHE_THREAD
#define MCC_TTY_NMAX_MSG_TO_SEND	8
#define MCC_TTY_BUFFER_SEND_SIZE	512
#define MCC_TTY_BUFFER_SEND_SIZE_PL	(MCC_TTY_BUFFER_SEND_SIZE-24)
#endif

/**
 * struct mcctty_port - Wrapper struct for imx mcc tty port.
 * @port:		TTY port data
 */
struct mcctty_port {
	struct delayed_work	read;
	struct tty_port		port;
	spinlock_t		rx_lock;
};

static struct mcctty_port mcc_tty_port;

enum {
	MCC_NODE_A9 = 0,
	MCC_NODE_M4 = 0,

	MCC_A9_PORT = 1,
	MCC_M4_PORT = 2,
};

/* mcc endpoint */
static MCC_ENDPOINT mcc_endpoint_a9 = {0, MCC_NODE_A9, MCC_A9_PORT};
static MCC_ENDPOINT mcc_endpoint_m4 = {1, MCC_NODE_M4, MCC_M4_PORT};

// used for receive messages
struct mcc_tty_msg {
	char data[MCC_ATTR_BUFFER_SIZE_IN_BYTES - 24];
	uint16_t dummy;	// for zero terminator
};

#ifdef SEND_IS_INTHE_THREAD
// used for send messages
struct mcc_tty_msg_out {
	char data[MCC_TTY_BUFFER_SEND_SIZE_PL];
	uint16_t count;
};
static struct mcc_tty_msg_out tty_msg_to_send[MCC_TTY_NMAX_MSG_TO_SEND];
#endif

static int mccNumMsgToSend = 0;
static int mccMsgOutPtr = 0;
static int mccMsgInPtr = 0;

static struct tty_port_operations  mcctty_port_ops = { };


struct task_struct *task;
int data;
int ret;
int thread_loop = 1;

//#define RECEIVE_WITH_MSG_AVAILABLE

// thread function listen for new message from M4 endpoint
#ifndef RECEIVE_WITH_MSG_AVAILABLE
int thread_function(void *data)
{
	int ret = 0, space;
	unsigned char *cbuf;
	MCC_MEM_SIZE num_of_received_bytes;
	struct mcc_tty_msg tty_msg;
	struct mcctty_port *cport = &mcc_tty_port;

	//printk(KERN_INFO"Enter into function thread!!\n");

	while(thread_loop){

#ifdef SEND_IS_INTHE_THREAD
		ret = mcc_recv(&mcc_endpoint_m4,
			       &mcc_endpoint_a9, &tty_msg,
			       sizeof(struct mcc_tty_msg),
			       &num_of_received_bytes, 0);
#else
		ret = mcc_recv(&mcc_endpoint_m4,
			       &mcc_endpoint_a9, &tty_msg,
			       sizeof(struct mcc_tty_msg),
			       &num_of_received_bytes, 0xffffffff);
#endif

		if (MCC_SUCCESS == ret) {
			if (num_of_received_bytes > 0) {
				//printk(KERN_INFO"ttymcc rx_bytes = %d!!\n", num_of_received_bytes);
				/* flush the recv-ed data to tty node */
				tty_msg.data[num_of_received_bytes++] = 0;
				spin_lock_bh(&cport->rx_lock);
				space = tty_prepare_flip_string(&cport->port, &cbuf,
								strlen(tty_msg.data));
				if (space <= 0)
					return -ENOMEM;

				//printk(KERN_INFO"num_of_received_bytes = %d!!\n", num_of_received_bytes);

				memcpy(cbuf, &tty_msg.data, num_of_received_bytes);
				//memcpy(cbuf, &tty_msg.data, strlen(tty_msg.data));
				tty_flip_buffer_push(&cport->port);
				spin_unlock_bh(&cport->rx_lock);
			}
		}

#ifdef SEND_IS_INTHE_THREAD
		// send section
		if (mccNumMsgToSend > 0) {
			//printk(KERN_INFO"thread_function - mccNumMsgToSend %d  mccMsgOutPtr %d\n", mccNumMsgToSend, mccMsgOutPtr);

			ret = mcc_send(&mcc_endpoint_a9,
				       &mcc_endpoint_m4, &tty_msg_to_send[mccMsgOutPtr],
				       //sizeof(struct mcc_tty_msg),
				       tty_msg_to_send[mccMsgOutPtr].count,
				       0);

			if (MCC_SUCCESS == ret) {
				//fefr
				//printk(KERN_INFO"mcc thread_function - send OK\n");
				mccNumMsgToSend--;
				mccMsgOutPtr++;
				if (mccMsgOutPtr >= MCC_TTY_NMAX_MSG_TO_SEND) mccMsgOutPtr = 0;
			}
			//fefr
			//else
			//printk(KERN_INFO"mcc thread_function - send KO\n");
		}
#endif
	}

	//printk(KERN_INFO"Exit da function thread!!\n");

	do_exit(0);
	return (0);
}
#else
int thread_function(void *data)
{
	int ret = 0, space, num_msgs;
	unsigned char *cbuf;
	MCC_MEM_SIZE num_of_received_bytes;
	struct mcc_tty_msg tty_msg;
	struct mcctty_port *cport = &mcc_tty_port;

	printk(KERN_INFO"ttymcc - enter into function thread with msgs available!!\n");

	while(thread_loop){

		if ((MCC_SUCCESS == mcc_msgs_available(&mcc_endpoint_a9, &num_msgs))) {
			// crash when M4 send message !!!
			if (num_msgs > 0) {
				//printk(KERN_INFO"ttymcc: num_msgs available=%d\n", num_msgs);

				do {
					ret = mcc_recv(&mcc_endpoint_m4,
						       &mcc_endpoint_a9, &tty_msg,
						       sizeof(struct mcc_tty_msg),
						       &num_of_received_bytes, 0);


					if (MCC_SUCCESS == ret) {
						if (num_of_received_bytes > 0) {
							/* flush the recv-ed data to tty node */
							tty_msg.data[num_of_received_bytes++] = 0;
							spin_lock_bh(&cport->rx_lock);
							space = tty_prepare_flip_string(&cport->port, &cbuf,
											strlen(tty_msg.data));
							if (space <= 0)
								return -ENOMEM;

							//printk(KERN_INFO"num_of_received_bytes = %d!!\n", num_of_received_bytes);

							memcpy(cbuf, &tty_msg.data, num_of_received_bytes);
							//memcpy(cbuf, &tty_msg.data, strlen(tty_msg.data));
							tty_flip_buffer_push(&cport->port);
							spin_unlock_bh(&cport->rx_lock);
						}
					}
					num_msgs--;
				} while (num_msgs > 0);
			}
		}
	}

	//printk(KERN_INFO"Exit da function thread!!\n");

	do_exit(0);
	return (0);
}
#endif

static int ttymcc_thread_init(void)
{
	data = 20;
	thread_loop = 1;
	printk(KERN_INFO"ttymcc_thread_init!!\n");
	//    task = kthread_create(&thread_function,(void *)data,"pradeep");
	task = kthread_run(&thread_function,(void *)data,"pradeep");
	printk(KERN_INFO"Kernel Thread : %s\n",task->comm);
	return 0;
}

static void ttymcc_thread_exit(void)
{
	printk(KERN_INFO"ttymcc_thread_exit!!\n");

	//printk(KERN_INFO"ttymcc_version_sttring=");
	//printk(bookeeping_data->init_string);
	//printk(KERN_INFO"\n");

	thread_loop = 0;
}



static int mcctty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	return tty_port_install(&mcc_tty_port.port, driver, tty);
}

static int mcctty_open(struct tty_struct *tty, struct file *filp)
{
	mccNumMsgToSend = 0;
	mccMsgOutPtr = 0;
	mccMsgInPtr = 0;
	ttymcc_thread_init();
	return tty_port_open(tty->port, tty, filp);
}

static void mcctty_close(struct tty_struct *tty, struct file *filp)
{
	ttymcc_thread_exit();
	return tty_port_close(tty->port, tty, filp);
}

#ifdef SEND_IS_INTHE_THREAD
static int mcctty_write(struct tty_struct *tty, const unsigned char *buf,
			int total)
{
	int i, count = 0;
	unsigned char *tmp;
	//struct mcc_tty_msg tty_msg;

	if (NULL == buf) {
		pr_err("buf shouldn't be null.\n");
		return -ENOMEM;
	}
	//fefr
	//printk(KERN_INFO"mcctty_write - total=%d\n", total);

	count = total;
	tmp = (unsigned char *)buf;
	for (i = 0; i <= count / (MCC_TTY_BUFFER_SEND_SIZE_PL-1); i++) {
		//printk(KERN_INFO"mcctty_write - mccNumMsgToSend,mccMsgInPtr %d,%d\n", mccNumMsgToSend, mccMsgInPtr);
		if (mccNumMsgToSend < MCC_TTY_NMAX_MSG_TO_SEND) {
			strlcpy(tty_msg_to_send[mccMsgInPtr].data, tmp, count >= (MCC_TTY_BUFFER_SEND_SIZE_PL) ? (MCC_TTY_BUFFER_SEND_SIZE_PL) : count + 1);
			tty_msg_to_send[mccMsgInPtr].count = count;
			mccNumMsgToSend++;
			mccMsgInPtr++;
			if (mccMsgInPtr >= MCC_TTY_NMAX_MSG_TO_SEND)
				mccMsgInPtr = 0;
		}
		else {
			printk(KERN_INFO"mcctty_write - list messages to send is full!\n");
		}

		if (count >= (MCC_TTY_BUFFER_SEND_SIZE_PL))
			count -= (MCC_TTY_BUFFER_SEND_SIZE_PL-1);

	}
	return total;
}
#else
static int mcctty_write(struct tty_struct *tty, const unsigned char *buf,
			 int total)
{
	int i, count, ret = 0;
	unsigned char *tmp;
	struct mcc_tty_msg tty_msg;

	if (NULL == buf) {
		pr_err("buf shouldn't be null.\n");
		return -ENOMEM;
	}

	count = total;
	tmp = (unsigned char *)buf;
	for (i = 0; i <= count / 999; i++) {
		strlcpy(tty_msg.data, tmp, count >= 1000 ? 1000 : count + 1);
		if (count >= 1000)
			count -= 999;

		/*
		 * wait until the remote endpoint is created by
		 * the other core
		 */
		ret = mcc_send(&mcc_endpoint_a9,
				&mcc_endpoint_m4, &tty_msg,
				//sizeof(struct mcc_tty_msg),
				count,
				1000);

		if (MCC_SUCCESS != ret)
			pr_err("A9 mcctty write error: %d\n", ret);

	}
	return total;
}
#endif

static int mcctty_write_room(struct tty_struct *tty)
{
	/* report the space in the mcc buffer */
	return MCC_ATTR_BUFFER_SIZE_IN_BYTES;
}

static const struct tty_operations imxmcctty_ops = {
	.install		= mcctty_install,
	.open			= mcctty_open,
	.close			= mcctty_close,
	.write			= mcctty_write,
	.write_room		= mcctty_write_room,
};

static struct tty_driver *mcctty_driver;

static int imx_mcc_tty_probe(struct platform_device *pdev)
{
	int ret;
	struct mcctty_port *cport = &mcc_tty_port;
	MCC_INFO_STRUCT mcc_info;

	mcctty_driver = tty_alloc_driver(1,
					 TTY_DRIVER_RESET_TERMIOS |
					 TTY_DRIVER_UNNUMBERED_NODE);
	if (IS_ERR(mcctty_driver))
		return PTR_ERR(mcctty_driver);

	mcctty_driver->driver_name = "mcc_tty";
	mcctty_driver->name = "ttyMCC";
	mcctty_driver->major = TTYAUX_MAJOR;
	mcctty_driver->minor_start = 3;
	mcctty_driver->type = TTY_DRIVER_TYPE_CONSOLE;
	mcctty_driver->init_termios = tty_std_termios;
	mcctty_driver->init_termios.c_cflag |= CLOCAL;

	tty_set_operations(mcctty_driver, &imxmcctty_ops);

	tty_port_init(&cport->port);
	cport->port.ops = &mcctty_port_ops;
	spin_lock_init(&cport->rx_lock);
	cport->port.low_latency = cport->port.flags | ASYNC_LOW_LATENCY;

	ret = tty_register_driver(mcctty_driver);
	if (ret < 0) {
		pr_err("Couldn't install mcc tty driver: err %d\n", ret);
		goto error;
	} else
		pr_info("Install mcc tty driver!\n");

	ret = mcc_initialize(MCC_NODE_A9);
	if (ret) {
		pr_err("failed to initialize mcc.\n");
		ret = -ENODEV;
		goto error;
	}

	ret = mcc_get_info(MCC_NODE_A9, &mcc_info);
	if (ret) {
		pr_err("failed to get mcc info.\n");
		ret = -ENODEV;
		goto error;
	} else {
		pr_info("\nA9 mcc prepares run, MCC version is %s\n",
			mcc_info.version_string);
		//pr_info("imx mcc tty/pingpong test begin.\n");
	}

	ret = mcc_create_endpoint(&mcc_endpoint_a9,
				  MCC_A9_PORT);
	if (ret) {
		pr_err("failed to create a9 mcc ep.\n");
		ret = -ENODEV;
		goto error;
	}

	return 0;

error:
	tty_unregister_driver(mcctty_driver);
	put_tty_driver(mcctty_driver);
	tty_port_destroy(&cport->port);
	mcctty_driver = NULL;

	return ret;
}

static int imx_mcc_tty_remove(struct platform_device *pdev)
{
	int ret = 0;
	struct mcctty_port *cport = &mcc_tty_port;

	/* destory the mcc tty endpoint here */
	ret = mcc_destroy_endpoint(&mcc_endpoint_a9);
	if (ret)
		pr_err("failed to destory a9 mcc ep.\n");
	else
		pr_info("destory a9 mcc ep.\n");

	tty_unregister_driver(mcctty_driver);
	tty_port_destroy(&cport->port);
	put_tty_driver(mcctty_driver);

	return ret;
}

static const struct of_device_id imx6sx_mcc_tty_ids[] = {
	{ .compatible = "fsl,imx6sx-mcc-tty", },
	{ /* sentinel */ }
};

static struct platform_driver imxmcctty_driver = {
	.driver = {
		.name = "imx6sx-mcc-tty",
		.owner  = THIS_MODULE,
		.of_match_table = imx6sx_mcc_tty_ids,
	},
	.probe = imx_mcc_tty_probe,
	.remove = imx_mcc_tty_remove,
};

/*!
 * Initialise the imxmcctty_driver.
 *
 * @return  The function always returns 0.
 */

static int __init imxmcctty_init(void)
{
	if (platform_driver_register(&imxmcctty_driver) != 0)
		return -ENODEV;

	printk(KERN_INFO "IMX MCC TTY driver module loaded\n");
	return 0;
}

static void __exit imxmcctty_exit(void)
{
	/* Unregister the device structure */
	platform_driver_unregister(&imxmcctty_driver);
}

module_init(imxmcctty_init);
module_exit(imxmcctty_exit);

MODULE_AUTHOR("Seco S.r.L.");
MODULE_DESCRIPTION("IMX M4 UDOONEO DEBUGGER, BASED ON MCC TTY");
MODULE_LICENSE("GPL");

