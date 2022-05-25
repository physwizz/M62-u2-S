/*
 * Changes:
 * Arnaldo Carvalho de Melo <acme@conectiva.com.br> 08/23/2000
 * - get rid of some verify_areas and use __copy*user and __get/put_user
 *   for the ones that remain
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>
#include <scsi/scsi_dbg.h>
#include <linux/compat.h>

#include "scsi_logging.h"

#define NORMAL_RETRIES			5
#define IOCTL_NORMAL_TIMEOUT			(10 * HZ)

#define MAX_BUF PAGE_SIZE
#define MAX_BUFFLEN	(32 * 512)

/**
 * ioctl_probe  --  return host identification
 * @host:	host to identify
 * @buffer:	userspace buffer for identification
 *
 * Return an identifying string at @buffer, if @buffer is non-NULL, filling
 * to the length stored at * (int *) @buffer.
 */
static int ioctl_probe(struct Scsi_Host *host, void __user *buffer)
{
	unsigned int len, slen;
	const char *string;

	if (buffer) {
		if (get_user(len, (unsigned int __user *) buffer))
			return -EFAULT;

		if (host->hostt->info)
			string = host->hostt->info(host);
		else
			string = host->hostt->name;
		if (string) {
			slen = strlen(string);
			if (len > slen)
				len = slen + 1;
			if (copy_to_user(buffer, string, len))
				return -EFAULT;
		}
	}
	return 1;
}

/*

 * The SCSI_IOCTL_SEND_COMMAND ioctl sends a command out to the SCSI host.
 * The IOCTL_NORMAL_TIMEOUT and NORMAL_RETRIES  variables are used.
 *
 * dev is the SCSI device struct ptr, *(int *) arg is the length of the
 * input data, if any, not including the command string & counts,
 * *((int *)arg + 1) is the output buffer size in bytes.
 *
 * *(char *) ((int *) arg)[2] the actual command byte.
 *
 * Note that if more than MAX_BUF bytes are requested to be transferred,
 * the ioctl will fail with error EINVAL.
 *
 * This size *does not* include the initial lengths that were passed.
 *
 * The SCSI command is read from the memory location immediately after the
 * length words, and the input data is right after the command.  The SCSI
 * routines know the command size based on the opcode decode.
 *
 * The output area is then filled in starting from the command byte.
 */

static int ioctl_internal_command(struct scsi_device *sdev, char *cmd,
				  int timeout, int retries)
{
	int result;
	struct scsi_sense_hdr sshdr;

	SCSI_LOG_IOCTL(1, sdev_printk(KERN_INFO, sdev,
				      "Trying ioctl with scsi command %d\n", *cmd));

	result = scsi_execute_req(sdev, cmd, DMA_NONE, NULL, 0,
				  &sshdr, timeout, retries, NULL);

	SCSI_LOG_IOCTL(2, sdev_printk(KERN_INFO, sdev,
				      "Ioctl returned  0x%x\n", result));

	if ((driver_byte(result) & DRIVER_SENSE) &&
	    (scsi_sense_valid(&sshdr))) {
		switch (sshdr.sense_key) {
		case ILLEGAL_REQUEST:
			if (cmd[0] == ALLOW_MEDIUM_REMOVAL)
				sdev->lockable = 0;
			else
				sdev_printk(KERN_INFO, sdev,
					    "ioctl_internal_command: "
					    "ILLEGAL REQUEST "
					    "asc=0x%x ascq=0x%x\n",
					    sshdr.asc, sshdr.ascq);
			break;
		case NOT_READY:	/* This happens if there is no disc in drive */
			if (sdev->removable)
				break;
			/* FALLTHROUGH */
		case UNIT_ATTENTION:
			if (sdev->removable) {
				sdev->changed = 1;
				result = 0;	/* This is no longer considered an error */
				break;
			}
			/* FALLTHROUGH -- for non-removable media */
		default:
			sdev_printk(KERN_INFO, sdev,
				    "ioctl_internal_command return code = %x\n",
				    result);
			scsi_print_sense_hdr(sdev, NULL, &sshdr);
			break;
		}
	}

	SCSI_LOG_IOCTL(2, sdev_printk(KERN_INFO, sdev,
				      "IOCTL Releasing command\n"));
	return result;
}

#if defined(CONFIG_UFS_SRPMB)
static int srpmb_ioctl_secu_prot_command(struct scsi_device *sdev, char *cmd,
					Rpmb_Req *req,
					int timeout, int retries)
{
	int result, dma_direction;
	struct scsi_sense_hdr sshdr;
	unsigned char *buf = NULL;
	unsigned int bufflen;
	int prot_in_out = req->cmd;

	SCSI_LOG_IOCTL(1, printk("Trying ioctl with scsi command %d\n", *cmd));
	if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_IN) {
		dma_direction = DMA_FROM_DEVICE;
		bufflen = req->inlen;
		if (bufflen <= 0 || bufflen > MAX_BUFFLEN) {
			sdev_printk(KERN_INFO, sdev,
					"Invalid bufflen : %x\n", bufflen);
			result = -EFAULT;
			goto err_pre_buf_alloc;
		}
		buf = kzalloc(bufflen, GFP_KERNEL);
		if (!virt_addr_valid(buf)) {
			result = -ENOMEM;
			goto err_kzalloc;
		}
	} else if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_OUT) {
		dma_direction = DMA_TO_DEVICE;
		bufflen = req->outlen;
		if (bufflen <= 0 || bufflen > MAX_BUFFLEN) {
			sdev_printk(KERN_INFO, sdev,
					"Invalid bufflen : %x\n", bufflen);
			result = -EFAULT;
			goto err_pre_buf_alloc;
		}
		buf = kzalloc(bufflen, GFP_KERNEL);
		if (!virt_addr_valid(buf)) {
			result = -ENOMEM;
			goto err_kzalloc;
		}
		memcpy(buf, req->rpmb_data, bufflen);
	} else {
		sdev_printk(KERN_INFO, sdev,
				"prot_in_out not set!! %d\n", prot_in_out);
		result = -EFAULT;
		goto err_pre_buf_alloc;
	}

	result = scsi_execute_req(sdev, cmd, dma_direction, buf, bufflen,
				  &sshdr, timeout, retries, NULL);

	if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_IN) {
		memcpy(req->rpmb_data, buf, bufflen);
	}
	SCSI_LOG_IOCTL(2, printk("Ioctl returned  0x%x\n", result));

	if ((driver_byte(result) & DRIVER_SENSE) &&
	    (scsi_sense_valid(&sshdr))) {
		sdev_printk(KERN_INFO, sdev,
			    "ioctl_secu_prot_command return code = %x\n",
			    result);
		scsi_print_sense_hdr(sdev, NULL, &sshdr);
	}

	kfree(buf);
err_pre_buf_alloc:
	SCSI_LOG_IOCTL(2, printk("IOCTL Releasing command\n"));
	return result;
err_kzalloc:
	if (buf)
		kfree(buf);
	printk(KERN_INFO "%s kzalloc faild\n", __func__);
	return result;
}
#endif

static int ioctl_secu_prot_command(struct scsi_device *sdev, char *cmd,
					int prot_in_out, void __user *arg,
					int timeout, int retries)
{
	int result, dma_direction;
	struct scsi_sense_hdr sshdr;
	unsigned char *buf;
	unsigned int bufflen;
	Scsi_Ioctl_Command __user *s_ioc_arg;

	SCSI_LOG_IOCTL(1, printk("Trying ioctl with scsi command %d\n", *cmd));

	s_ioc_arg = (Scsi_Ioctl_Command *)kmalloc(sizeof(*s_ioc_arg), GFP_KERNEL);
	if (!s_ioc_arg) {
		printk(KERN_INFO "%s kmalloc faild\n", __func__);
		return -EFAULT;
	}


	if (copy_from_user(s_ioc_arg, arg, sizeof(*s_ioc_arg))) {
		printk(KERN_INFO "Argument copy faild\n");
		result = -EFAULT;
		goto err_pre_buf_alloc;
	}
	if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_IN) {
		dma_direction = DMA_FROM_DEVICE;
		bufflen = s_ioc_arg->inlen;
		if (bufflen <= 0 || bufflen > MAX_BUFFLEN) {
			sdev_printk(KERN_INFO, sdev,
					"Invalid bufflen : %x\n", bufflen);
			result = -EFAULT;
			goto err_pre_buf_alloc;
		}
		buf = kzalloc(bufflen, GFP_KERNEL);
	} else if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_OUT) {
		dma_direction = DMA_TO_DEVICE;
		bufflen = s_ioc_arg->outlen;
		if (bufflen <= 0 || bufflen > MAX_BUFFLEN) {
			sdev_printk(KERN_INFO, sdev,
					"Invalid bufflen : %x\n", bufflen);
			result = -EFAULT;
			goto err_pre_buf_alloc;
		}
		buf = kzalloc(bufflen, GFP_KERNEL);
		if (copy_from_user(buf, arg + sizeof(*s_ioc_arg), s_ioc_arg->outlen)) {
			printk(KERN_INFO "copy_from_user failed\n");
			result = -EFAULT;
			goto err_post_buf_alloc;
		}
	} else {
		sdev_printk(KERN_INFO, sdev,
				"prot_in_out not set!! %d\n", prot_in_out);
		result = -EFAULT;
		goto err_pre_buf_alloc;
	}

	result = scsi_execute_req(sdev, cmd, dma_direction, buf, bufflen,
				  &sshdr, timeout, retries, NULL);

	if (prot_in_out == SCSI_IOCTL_SECURITY_PROTOCOL_IN) {
		if (copy_to_user(arg + sizeof(*s_ioc_arg), buf, s_ioc_arg->inlen)) {
			printk(KERN_INFO "copy_to_user failed\n");
			result = -EFAULT;
			goto err_post_buf_alloc;
		}
	}
	SCSI_LOG_IOCTL(2, printk("Ioctl returned  0x%x\n", result));

	if ((driver_byte(result) & DRIVER_SENSE) &&
	    (scsi_sense_valid(&sshdr))) {
		sdev_printk(KERN_INFO, sdev,
			    "ioctl_secu_prot_command return code = %x\n",
			    result);
		scsi_print_sense_hdr(sdev, NULL, &sshdr);
	}

err_post_buf_alloc:
	kfree(buf);
err_pre_buf_alloc:
	kfree(s_ioc_arg);
	SCSI_LOG_IOCTL(2, printk("IOCTL Releasing command\n"));
	return result;
}

int scsi_set_medium_removal(struct scsi_device *sdev, char state)
{
	char scsi_cmd[MAX_COMMAND_SIZE];
	int ret;

	if (!sdev->removable || !sdev->lockable)
	       return 0;

	scsi_cmd[0] = ALLOW_MEDIUM_REMOVAL;
	scsi_cmd[1] = 0;
	scsi_cmd[2] = 0;
	scsi_cmd[3] = 0;
	scsi_cmd[4] = state;
	scsi_cmd[5] = 0;

	ret = ioctl_internal_command(sdev, scsi_cmd,
			IOCTL_NORMAL_TIMEOUT, NORMAL_RETRIES);
	if (ret == 0)
		sdev->locked = (state == SCSI_REMOVAL_PREVENT);
	return ret;
}
EXPORT_SYMBOL(scsi_set_medium_removal);

/*
 * The scsi_ioctl_get_pci() function places into arg the value
 * pci_dev::slot_name (8 characters) for the PCI device (if any).
 * Returns: 0 on success
 *          -ENXIO if there isn't a PCI device pointer
 *                 (could be because the SCSI driver hasn't been
 *                  updated yet, or because it isn't a SCSI
 *                  device)
 *          any copy_to_user() error on failure there
 */
static int scsi_ioctl_get_pci(struct scsi_device *sdev, void __user *arg)
{
	struct device *dev = scsi_get_device(sdev->host);
	const char *name;

        if (!dev)
		return -ENXIO;

	name = dev_name(dev);

	/* compatibility with old ioctl which only returned
	 * 20 characters */
        return copy_to_user(arg, name, min(strlen(name), (size_t)20))
		? -EFAULT: 0;
}

#if defined(CONFIG_UFS_SRPMB)
/**
 * srpmb_scsi_ioctl - Dispatch rpmb ioctl to scsi device
 * @sdev: scsi device receiving srpmb_worker
 * @req: rpmb struct with srpmb_worker
 *
 * Description: The scsi_ioctl() function differs from most ioctls in that it
 * does not take a major/minor number as the dev field.  Rather, it takes
 * a pointer to a &struct scsi_device.
 */
int srpmb_scsi_ioctl(struct scsi_device *sdev, Rpmb_Req *req)
{
	char scsi_cmd[MAX_COMMAND_SIZE];
	unsigned short prot_spec;
	unsigned long t_len;
	int _cmd;

	/* No idea how this happens.... */
	if (!sdev) {
		printk(KERN_ERR "sdev empty\n");
		return -ENXIO;
	}

	memset(scsi_cmd, 0x0, MAX_COMMAND_SIZE);
	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	if (!scsi_block_when_processing_errors(sdev))
		return -ENODEV;

	_cmd = SCSI_UFS_REQUEST_SENSE;
	if (sdev->host->wlun_clr_uac)
		sdev->host->hostt->ioctl(sdev, _cmd, NULL);

	prot_spec = SECU_PROT_SPEC_CERT_DATA;
	if (req->cmd == SCSI_IOCTL_SECURITY_PROTOCOL_IN)
		t_len = req->inlen;
	else
		t_len = req->outlen;

	scsi_cmd[0] = (req->cmd == SCSI_IOCTL_SECURITY_PROTOCOL_IN) ?
		SECURITY_PROTOCOL_IN :
		SECURITY_PROTOCOL_OUT;
	scsi_cmd[1] = SECU_PROT_UFS;
	scsi_cmd[2] = ((unsigned char)(prot_spec >> 8)) & 0xff;
	scsi_cmd[3] = ((unsigned char)(prot_spec)) & 0xff;
	scsi_cmd[4] = 0;
	scsi_cmd[5] = 0;
	scsi_cmd[6] = ((unsigned char)(t_len >> 24)) & 0xff;
	scsi_cmd[7] = ((unsigned char)(t_len >> 16)) & 0xff;
	scsi_cmd[8] = ((unsigned char)(t_len >> 8)) & 0xff;
	scsi_cmd[9] = (unsigned char)t_len & 0xff;
	scsi_cmd[10] = 0;
	scsi_cmd[11] = 0;
	return srpmb_ioctl_secu_prot_command(sdev, scsi_cmd,
			req,
			START_STOP_TIMEOUT, NORMAL_RETRIES);
}
#endif

/**
 * scsi_ioctl - Dispatch ioctl to scsi device
 * @sdev: scsi device receiving ioctl
 * @cmd: which ioctl is it
 * @arg: data associated with ioctl
 *
 * Description: The scsi_ioctl() function differs from most ioctls in that it
 * does not take a major/minor number as the dev field.  Rather, it takes
 * a pointer to a &struct scsi_device.
 */
int scsi_ioctl(struct scsi_device *sdev, int cmd, void __user *arg)
{
	char scsi_cmd[MAX_COMMAND_SIZE];
	struct scsi_sense_hdr sense_hdr;
	struct scsi_host_template *sht = sdev->host->hostt;

	memset(scsi_cmd, 0x0, MAX_COMMAND_SIZE);

	/* Check for deprecated ioctls ... all the ioctls which don't
	 * follow the new unique numbering scheme are deprecated */
	switch (cmd) {
	case SCSI_IOCTL_SEND_COMMAND:
	case SCSI_IOCTL_TEST_UNIT_READY:
	case SCSI_IOCTL_BENCHMARK_COMMAND:
	case SCSI_IOCTL_SYNC:
	case SCSI_IOCTL_START_UNIT:
	case SCSI_IOCTL_STOP_UNIT:
		printk(KERN_WARNING "program %s is using a deprecated SCSI "
		       "ioctl, please convert it to SG_IO\n", current->comm);
		break;
	default:
		break;
	}

	switch (cmd) {
	case SCSI_IOCTL_GET_IDLUN:
		if (!access_ok(VERIFY_WRITE, arg, sizeof(struct scsi_idlun)))
			return -EFAULT;

		__put_user((sdev->id & 0xff)
			 + ((sdev->lun & 0xff) << 8)
			 + ((sdev->channel & 0xff) << 16)
			 + ((sdev->host->host_no & 0xff) << 24),
			 &((struct scsi_idlun __user *)arg)->dev_id);
		__put_user(sdev->host->unique_id,
			 &((struct scsi_idlun __user *)arg)->host_unique_id);
		return 0;
	case SCSI_IOCTL_GET_BUS_NUMBER:
		return put_user(sdev->host->host_no, (int __user *)arg);
	case SCSI_IOCTL_PROBE_HOST:
		return ioctl_probe(sdev->host, arg);
	case SCSI_IOCTL_SEND_COMMAND:
		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
			return -EACCES;
		return sg_scsi_ioctl(sdev->request_queue, NULL, 0, arg);
	case SCSI_IOCTL_DOORLOCK:
		return scsi_set_medium_removal(sdev, SCSI_REMOVAL_PREVENT);
	case SCSI_IOCTL_DOORUNLOCK:
		return scsi_set_medium_removal(sdev, SCSI_REMOVAL_ALLOW);
	case SCSI_IOCTL_TEST_UNIT_READY:
		return scsi_test_unit_ready(sdev, IOCTL_NORMAL_TIMEOUT,
					    NORMAL_RETRIES, &sense_hdr);
	case SCSI_IOCTL_START_UNIT:
		scsi_cmd[0] = START_STOP;
		scsi_cmd[1] = 0;
		scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
		scsi_cmd[4] = 1;
		return ioctl_internal_command(sdev, scsi_cmd,
				     START_STOP_TIMEOUT, NORMAL_RETRIES);
	case SCSI_IOCTL_STOP_UNIT:
		scsi_cmd[0] = START_STOP;
		scsi_cmd[1] = 0;
		scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
		scsi_cmd[4] = 0;
		return ioctl_internal_command(sdev, scsi_cmd,
				     START_STOP_TIMEOUT, NORMAL_RETRIES);
	case SCSI_IOCTL_SECURITY_PROTOCOL_IN:
	case SCSI_IOCTL_SECURITY_PROTOCOL_OUT:
		{
			unsigned short prot_spec;
			unsigned long t_len;
			int _cmd;
			struct scsi_ioctl_command *ic = (struct scsi_ioctl_command *) arg;

			_cmd = SCSI_UFS_REQUEST_SENSE;
			if(sdev->host->wlun_clr_uac)
				sdev->host->hostt->ioctl(sdev, _cmd, NULL);

			prot_spec = SECU_PROT_SPEC_CERT_DATA;
			if(cmd == SCSI_IOCTL_SECURITY_PROTOCOL_IN)
				t_len = ic->inlen;
			else
				t_len = ic->outlen;

			scsi_cmd[0] = (cmd == SCSI_IOCTL_SECURITY_PROTOCOL_IN) ?
				SECURITY_PROTOCOL_IN :
				SECURITY_PROTOCOL_OUT;
			scsi_cmd[1] = SECU_PROT_UFS;
			scsi_cmd[2] = ((unsigned char)(prot_spec >> 8)) & 0xff;
			scsi_cmd[3] = ((unsigned char)(prot_spec)) & 0xff;
			scsi_cmd[4] = 0;
			scsi_cmd[5] = 0;
			scsi_cmd[6] = ((unsigned char)(t_len >> 24)) & 0xff;
			scsi_cmd[7] = ((unsigned char)(t_len >> 16)) & 0xff;
			scsi_cmd[8] = ((unsigned char)(t_len >> 8)) & 0xff;
			scsi_cmd[9] = (unsigned char)t_len & 0xff;
			scsi_cmd[10] = 0;
			scsi_cmd[11] = 0;
			return ioctl_secu_prot_command(sdev, scsi_cmd,
					cmd, arg,
					START_STOP_TIMEOUT, NORMAL_RETRIES);
		}

	case SCSI_IOCTL_GET_PCI:
                return scsi_ioctl_get_pci(sdev, arg);
	case SG_SCSI_RESET:
		if (!strncmp(sht->name, "ufshcd", 6))
			return -EINVAL;
		else
			return scsi_ioctl_reset(sdev, arg);
	default:
		if (sdev->host->hostt->ioctl)
			return sdev->host->hostt->ioctl(sdev, cmd, arg);
	}
	return -EINVAL;
}
EXPORT_SYMBOL(scsi_ioctl);

/*
 * We can process a reset even when a device isn't fully operable.
 */
int scsi_ioctl_block_when_processing_errors(struct scsi_device *sdev, int cmd,
		bool ndelay)
{
	if (cmd == SG_SCSI_RESET && ndelay) {
		if (scsi_host_in_recovery(sdev->host))
			return -EAGAIN;
	} else {
		if (!scsi_block_when_processing_errors(sdev))
			return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_ioctl_block_when_processing_errors);
