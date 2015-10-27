#include <linux/init.h>
#include <linux/mm.h>
#include <linux/security.h>
#include <linux/sysctl.h>

/* amount of vm to protect from userspace access by both DAC and the LSM*/
/** 20150912    
 * userspace의 접근으로부터 보호하기 위해 DAC와 LSM에서 사용할 최소 mmap 크기.
 **/
unsigned long mmap_min_addr;
/* amount of vm to protect from userspace using CAP_SYS_RAWIO (DAC) */
unsigned long dac_mmap_min_addr = CONFIG_DEFAULT_MMAP_MIN_ADDR;
/* amount of vm to protect from userspace using the LSM = CONFIG_LSM_MMAP_MIN_ADDR */

/*
 * Update mmap_min_addr = max(dac_mmap_min_addr, CONFIG_LSM_MMAP_MIN_ADDR)
 */
/** 20150912    
 * mmap의 최소 주소 크기를 LSM 또는 DAC에 따라 사용할 크기를 업데이트 한다.
 *
 * LSM : Linux Security Model
 * https://en.wikipedia.org/wiki/Linux_Security_Modules
 *
 * DAC : Discretionary access control
 * https://en.wikipedia.org/wiki/Discretionary_access_control
 **/
static void update_mmap_min_addr(void)
{
#ifdef CONFIG_LSM_MMAP_MIN_ADDR
	if (dac_mmap_min_addr > CONFIG_LSM_MMAP_MIN_ADDR)
		mmap_min_addr = dac_mmap_min_addr;
	else
		mmap_min_addr = CONFIG_LSM_MMAP_MIN_ADDR;
#else
	mmap_min_addr = dac_mmap_min_addr;
#endif
}

/*
 * sysctl handler which just sets dac_mmap_min_addr = the new value and then
 * calls update_mmap_min_addr() so non MAP_FIXED hints get rounded properly
 */
int mmap_min_addr_handler(struct ctl_table *table, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	if (write && !capable(CAP_SYS_RAWIO))
		return -EPERM;

	ret = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);

	update_mmap_min_addr();

	return ret;
}

/** 20151024    
 **/
static int __init init_mmap_min_addr(void)
{
	update_mmap_min_addr();

	return 0;
}
pure_initcall(init_mmap_min_addr);
