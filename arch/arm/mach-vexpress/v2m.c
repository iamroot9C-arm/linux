/*
 * Versatile Express V2M Motherboard Support
 */
#include <linux/device.h>
#include <linux/amba/bus.h>
#include <linux/amba/mmci.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/ata_platform.h>
#include <linux/smsc911x.h>
#include <linux/spinlock.h>
#include <linux/usb/isp1760.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/mtd/physmap.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>

#include <asm/arch_timer.h>
#include <asm/mach-types.h>
#include <asm/sizes.h>
#include <asm/smp_twd.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/time.h>
#include <asm/hardware/arm_timer.h>
#include <asm/hardware/cache-l2x0.h>
#include <asm/hardware/gic.h>
#include <asm/hardware/timer-sp.h>
#include <asm/hardware/sp810.h>

#include <mach/ct-ca9x4.h>
#include <mach/motherboard.h>

#include <plat/sched_clock.h>

#include "core.h"

#define V2M_PA_CS0	0x40000000
#define V2M_PA_CS1	0x44000000
#define V2M_PA_CS2	0x48000000
#define V2M_PA_CS3	0x4c000000
#define V2M_PA_CS7	0x10000000

static struct map_desc v2m_io_desc[] __initdata = {
	{
		.virtual	= V2M_PERIPH,
		.pfn		= __phys_to_pfn(V2M_PA_CS7),
		.length		= SZ_128K,
		.type		= MT_DEVICE,
	},
};

/** 20150103
 * V2M_SYSREGS의 가상 주소.
 **/
static void __iomem *v2m_sysreg_base;

/** 20141213
 * base주소를 읽어와서 base로부터의 offset에 해당하는 register를 초기화한다.
 **/
static void __init v2m_sysctl_init(void __iomem *base)
{
	u32 scctrl;

	if (WARN_ON(!base))
		return;

	/* Select 1MHz TIMCLK as the reference clock for SP804 timers */
	scctrl = readl(base + SCCTRL);
	scctrl |= SCCTRL_TIMEREN0SEL_TIMCLK;
	scctrl |= SCCTRL_TIMEREN1SEL_TIMCLK;
	writel(scctrl, base + SCCTRL);
}

/** 20150103
 * sp804의 레지스터를 설정하고,
 * clocksource와 clockevent로 등록한다.
 **/
static void __init v2m_sp804_init(void __iomem *base, unsigned int irq)
{
	if (WARN_ON(!base || irq == NO_IRQ))
		return;

	/** 20141227
	 * TIMER1, TIMER2의 TIMER_CTRL 레지스터에 값을 쓴다.
	 * 자세한 내용은 datasheet (DDI0271 sp804 trm) 참조.
	 **/
	writel(0, base + TIMER_1_BASE + TIMER_CTRL);
	writel(0, base + TIMER_2_BASE + TIMER_CTRL);

	/** 20141227
	 * "v2m-timer1"라는 이름으로 common clock framework에서 clk을 받아오고,
	 * clocksource로 사용하기 위해 register를 설정하고 clocksource 구조체를 설정해 등록한다.
	 * HW는 TIMER2와 연결한다.
	 *
	 * "/sys/devices/system/clocksource/current_clocksource"
	 **/
	sp804_clocksource_init(base + TIMER_2_BASE, "v2m-timer1");
	/** 20140830
	 * periodic tick발생용 timer.
	 **/
	sp804_clockevents_init(base + TIMER_1_BASE, irq, "v2m-timer0");
}


static DEFINE_SPINLOCK(v2m_cfg_lock);

int v2m_cfg_write(u32 devfn, u32 data)
{
	/* Configuration interface broken? */
	u32 val;

	printk("%s: writing %08x to %08x\n", __func__, data, devfn);

	devfn |= SYS_CFG_START | SYS_CFG_WRITE;

	spin_lock(&v2m_cfg_lock);
	val = readl(v2m_sysreg_base + V2M_SYS_CFGSTAT);
	writel(val & ~SYS_CFG_COMPLETE, v2m_sysreg_base + V2M_SYS_CFGSTAT);

	writel(data, v2m_sysreg_base +  V2M_SYS_CFGDATA);
	writel(devfn, v2m_sysreg_base + V2M_SYS_CFGCTRL);

	do {
		val = readl(v2m_sysreg_base + V2M_SYS_CFGSTAT);
	} while (val == 0);
	spin_unlock(&v2m_cfg_lock);

	return !!(val & SYS_CFG_ERR);
}

int v2m_cfg_read(u32 devfn, u32 *data)
{
	u32 val;

	devfn |= SYS_CFG_START;

	spin_lock(&v2m_cfg_lock);
	writel(0, v2m_sysreg_base + V2M_SYS_CFGSTAT);
	writel(devfn, v2m_sysreg_base + V2M_SYS_CFGCTRL);

	mb();

	do {
		cpu_relax();
		val = readl(v2m_sysreg_base + V2M_SYS_CFGSTAT);
	} while (val == 0);

	*data = readl(v2m_sysreg_base + V2M_SYS_CFGDATA);
	spin_unlock(&v2m_cfg_lock);

	return !!(val & SYS_CFG_ERR);
}

/** 20150613
 * sysreg_base register에 data를 저장한다.
 *	 versatile_secondary_startup의 물리주소.
 **/
void __init v2m_flags_set(u32 data)
{
	writel(~0, v2m_sysreg_base + V2M_SYS_FLAGSCLR);
	writel(data, v2m_sysreg_base + V2M_SYS_FLAGSSET);
}

int v2m_get_master_site(void)
{
	u32 misc = readl(v2m_sysreg_base + V2M_SYS_MISC);

	return misc & SYS_MISC_MASTERSITE ? SYS_CFG_SITE_DB2 : SYS_CFG_SITE_DB1;
}


static struct resource v2m_pcie_i2c_resource = {
	.start	= V2M_SERIAL_BUS_PCI,
	.end	= V2M_SERIAL_BUS_PCI + SZ_4K - 1,
	.flags	= IORESOURCE_MEM,
};

static struct platform_device v2m_pcie_i2c_device = {
	.name		= "versatile-i2c",
	.id		= 0,
	.num_resources	= 1,
	.resource	= &v2m_pcie_i2c_resource,
};

static struct resource v2m_ddc_i2c_resource = {
	.start	= V2M_SERIAL_BUS_DVI,
	.end	= V2M_SERIAL_BUS_DVI + SZ_4K - 1,
	.flags	= IORESOURCE_MEM,
};

static struct platform_device v2m_ddc_i2c_device = {
	.name		= "versatile-i2c",
	.id		= 1,
	.num_resources	= 1,
	.resource	= &v2m_ddc_i2c_resource,
};

static struct resource v2m_eth_resources[] = {
	{
		.start	= V2M_LAN9118,
		.end	= V2M_LAN9118 + SZ_64K - 1,
		.flags	= IORESOURCE_MEM,
	}, {
		.start	= IRQ_V2M_LAN9118,
		.end	= IRQ_V2M_LAN9118,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct smsc911x_platform_config v2m_eth_config = {
	.flags		= SMSC911X_USE_32BIT,
	.irq_polarity	= SMSC911X_IRQ_POLARITY_ACTIVE_HIGH,
	.irq_type	= SMSC911X_IRQ_TYPE_PUSH_PULL,
	.phy_interface	= PHY_INTERFACE_MODE_MII,
};

static struct platform_device v2m_eth_device = {
	.name		= "smsc911x",
	.id		= -1,
	.resource	= v2m_eth_resources,
	.num_resources	= ARRAY_SIZE(v2m_eth_resources),
	.dev.platform_data = &v2m_eth_config,
};

static struct regulator_consumer_supply v2m_eth_supplies[] = {
	REGULATOR_SUPPLY("vddvario", "smsc911x"),
	REGULATOR_SUPPLY("vdd33a", "smsc911x"),
};

static struct resource v2m_usb_resources[] = {
	{
		.start	= V2M_ISP1761,
		.end	= V2M_ISP1761 + SZ_128K - 1,
		.flags	= IORESOURCE_MEM,
	}, {
		.start	= IRQ_V2M_ISP1761,
		.end	= IRQ_V2M_ISP1761,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct isp1760_platform_data v2m_usb_config = {
	.is_isp1761		= true,
	.bus_width_16		= false,
	.port1_otg		= true,
	.analog_oc		= false,
	.dack_polarity_high	= false,
	.dreq_polarity_high	= false,
};

static struct platform_device v2m_usb_device = {
	.name		= "isp1760",
	.id		= -1,
	.resource	= v2m_usb_resources,
	.num_resources	= ARRAY_SIZE(v2m_usb_resources),
	.dev.platform_data = &v2m_usb_config,
};

static void v2m_flash_set_vpp(struct platform_device *pdev, int on)
{
	writel(on != 0, v2m_sysreg_base + V2M_SYS_FLASH);
}

static struct physmap_flash_data v2m_flash_data = {
	.width		= 4,
	.set_vpp	= v2m_flash_set_vpp,
};

static struct resource v2m_flash_resources[] = {
	{
		.start	= V2M_NOR0,
		.end	= V2M_NOR0 + SZ_64M - 1,
		.flags	= IORESOURCE_MEM,
	}, {
		.start	= V2M_NOR1,
		.end	= V2M_NOR1 + SZ_64M - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device v2m_flash_device = {
	.name		= "physmap-flash",
	.id		= -1,
	.resource	= v2m_flash_resources,
	.num_resources	= ARRAY_SIZE(v2m_flash_resources),
	.dev.platform_data = &v2m_flash_data,
};

static struct pata_platform_info v2m_pata_data = {
	.ioport_shift	= 2,
};

static struct resource v2m_pata_resources[] = {
	{
		.start	= V2M_CF,
		.end	= V2M_CF + 0xff,
		.flags	= IORESOURCE_MEM,
	}, {
		.start	= V2M_CF + 0x100,
		.end	= V2M_CF + SZ_4K - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device v2m_cf_device = {
	.name		= "pata_platform",
	.id		= -1,
	.resource	= v2m_pata_resources,
	.num_resources	= ARRAY_SIZE(v2m_pata_resources),
	.dev.platform_data = &v2m_pata_data,
};

static unsigned int v2m_mmci_status(struct device *dev)
{
	return readl(v2m_sysreg_base + V2M_SYS_MCI) & (1 << 0);
}

static struct mmci_platform_data v2m_mmci_data = {
	.ocr_mask	= MMC_VDD_32_33|MMC_VDD_33_34,
	.status		= v2m_mmci_status,
};

static AMBA_APB_DEVICE(aaci,  "mb:aaci",  0, V2M_AACI, IRQ_V2M_AACI, NULL);
static AMBA_APB_DEVICE(mmci,  "mb:mmci",  0, V2M_MMCI, IRQ_V2M_MMCI, &v2m_mmci_data);
static AMBA_APB_DEVICE(kmi0,  "mb:kmi0",  0, V2M_KMI0, IRQ_V2M_KMI0, NULL);
static AMBA_APB_DEVICE(kmi1,  "mb:kmi1",  0, V2M_KMI1, IRQ_V2M_KMI1, NULL);
static AMBA_APB_DEVICE(uart0, "mb:uart0", 0, V2M_UART0, IRQ_V2M_UART0, NULL);
static AMBA_APB_DEVICE(uart1, "mb:uart1", 0, V2M_UART1, IRQ_V2M_UART1, NULL);
static AMBA_APB_DEVICE(uart2, "mb:uart2", 0, V2M_UART2, IRQ_V2M_UART2, NULL);
static AMBA_APB_DEVICE(uart3, "mb:uart3", 0, V2M_UART3, IRQ_V2M_UART3, NULL);
static AMBA_APB_DEVICE(wdt,   "mb:wdt",   0, V2M_WDT, IRQ_V2M_WDT, NULL);
static AMBA_APB_DEVICE(rtc,   "mb:rtc",   0, V2M_RTC, IRQ_V2M_RTC, NULL);

/** 20151121
 * v2m에서 사용하는 amba 디바이스. amba bus에 등록된다.
 **/
static struct amba_device *v2m_amba_devs[] __initdata = {
	&aaci_device,
	&mmci_device,
	&kmi0_device,
	&kmi1_device,
	&uart0_device,
	&uart1_device,
	&uart2_device,
	&uart3_device,
	&wdt_device,
	&rtc_device,
};


static unsigned long v2m_osc_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct v2m_osc *osc = to_v2m_osc(hw);

	return !parent_rate ? osc->rate_default : parent_rate;
}

static long v2m_osc_round_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long *parent_rate)
{
	struct v2m_osc *osc = to_v2m_osc(hw);

	if (WARN_ON(rate < osc->rate_min))
		rate = osc->rate_min;

	if (WARN_ON(rate > osc->rate_max))
		rate = osc->rate_max;

	return rate;
}

static int v2m_osc_set_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	struct v2m_osc *osc = to_v2m_osc(hw);

	v2m_cfg_write(SYS_CFG_OSC | SYS_CFG_SITE(osc->site) |
			SYS_CFG_STACK(osc->stack) | osc->osc, rate);

	return 0;
}

/** 20141227
 * v2m OSC를 위한 callback operations.
 * common clock framework에서 호출되어 hardware에 의존적인 동작을 수행한다.
 **/
static struct clk_ops v2m_osc_ops = {
	.recalc_rate = v2m_osc_recalc_rate,
	.round_rate = v2m_osc_round_rate,
	.set_rate = v2m_osc_set_rate,
};

/** 20141227
 * OSC clock을 clk hierarchy에 추가한다.
 **/
struct clk * __init v2m_osc_register(const char *name, struct v2m_osc *osc)
{
	/** 20141227
	 * clock framework와 clk_hw 사이에 약속된 구조체를 채운다.
	 * 설정된 clk
	 **/
	struct clk_init_data init;

	WARN_ON(osc->site > 2);
	WARN_ON(osc->stack > 15);
	WARN_ON(osc->osc > 4095);

	init.name = name;
	init.ops = &v2m_osc_ops;
	init.flags = CLK_IS_ROOT;
	init.num_parents = 0;

	osc->hw.init = &init;

	return clk_register(NULL, &osc->hw);
}

static struct v2m_osc v2m_mb_osc1 = {
	.site = SYS_CFG_SITE_MB,
	.osc = 1,
	.rate_min = 23750000,
	.rate_max = 63500000,
	.rate_default = 23750000,
};

static const char *v2m_ref_clk_periphs[] __initconst = {
	"mb:wdt",   "1000f000.wdt",  "1c0f0000.wdt",	/* SP805 WDT */
};

static const char *v2m_osc1_periphs[] __initconst = {
	"mb:clcd",  "1001f000.clcd", "1c1f0000.clcd",	/* PL111 CLCD */
};

static const char *v2m_osc2_periphs[] __initconst = {
	"mb:mmci",  "10005000.mmci", "1c050000.mmci",	/* PL180 MMCI */
	"mb:kmi0",  "10006000.kmi",  "1c060000.kmi",	/* PL050 KMI0 */
	"mb:kmi1",  "10007000.kmi",  "1c070000.kmi",	/* PL050 KMI1 */
	"mb:uart0", "10009000.uart", "1c090000.uart",	/* PL011 UART0 */
	"mb:uart1", "1000a000.uart", "1c0a0000.uart",	/* PL011 UART1 */
	"mb:uart2", "1000b000.uart", "1c0b0000.uart",	/* PL011 UART2 */
	"mb:uart3", "1000c000.uart", "1c0c0000.uart",	/* PL011 UART3 */
};

/** 20141213
 * v2m의 clock device를 clk구조체를 clock framework에 등록한다. 
 **/

/** 20141227
 * v2m machine을 위한 clk 초기화 작업.
 *
 * clk 구조체를 생성해 clock hierarchy에 등록한다.
 * 등록한 clk을 검색하기 위한 clk_lookup 구조체를 생성해 리스트에 등록한다.
 **/
static void __init v2m_clk_init(void)
{
	struct clk *clk;
	int i;

	clk = clk_register_fixed_rate(NULL, "dummy_apb_pclk", NULL,
			CLK_IS_ROOT, 0);
	WARN_ON(clk_register_clkdev(clk, "apb_pclk", NULL));

	clk = clk_register_fixed_rate(NULL, "mb:ref_clk", NULL,
			CLK_IS_ROOT, 32768);
	for (i = 0; i < ARRAY_SIZE(v2m_ref_clk_periphs); i++)
		WARN_ON(clk_register_clkdev(clk, NULL, v2m_ref_clk_periphs[i]));

	clk = clk_register_fixed_rate(NULL, "mb:sp804_clk", NULL,
			CLK_IS_ROOT, 1000000);
	WARN_ON(clk_register_clkdev(clk, "v2m-timer0", "sp804"));
	WARN_ON(clk_register_clkdev(clk, "v2m-timer1", "sp804"));

	clk = v2m_osc_register("mb:osc1", &v2m_mb_osc1);
	for (i = 0; i < ARRAY_SIZE(v2m_osc1_periphs); i++)
		WARN_ON(clk_register_clkdev(clk, NULL, v2m_osc1_periphs[i]));

	clk = clk_register_fixed_rate(NULL, "mb:osc2", NULL,
			CLK_IS_ROOT, 24000000);
	for (i = 0; i < ARRAY_SIZE(v2m_osc2_periphs); i++)
		WARN_ON(clk_register_clkdev(clk, NULL, v2m_osc2_periphs[i]));
}

/** 20150103
 * v2m timer 초기화.
 *	- sysctl control에서 timer0, timer1의 TIMCLK을 선택한다.
 *	- clock hierarchy에 clk을 등록한다.
 *	- sp804를 clocksource와 clockevent로 등록한다.
 **/
static void __init v2m_timer_init(void)
{

	/** 20141227
	 * V2M_SYSCTL 레지스터 영역을 4KB만큼 page table에 매핑시키고,
	 * 레지스터를 초기화 한다.
	 **/
	v2m_sysctl_init(ioremap(V2M_SYSCTL, SZ_4K));
	/** 20150103
	 * clk을 구조체를 생성해 clock hierarchy에 등록하고,
	 * lookup용 자료구조에 추가한다.
	 **/
	v2m_clk_init();
	/** 20141227
	 * V2M TIMER0,1와 관련된 물리주소를 4KB만큼 페이지 테이블에 매핑시킨 뒤,
	 * 가상주소와 irq 번호를 매개변수로 sp804 init 함수를 호출한다.
	 **/
	v2m_sp804_init(ioremap(V2M_TIMER01, SZ_4K), IRQ_V2M_TIMER0);
}

/** 20140830
 * timer init 콜백함수 지정.
 **/
static struct sys_timer v2m_timer = {
	.init	= v2m_timer_init,
};

/** 20130601
 * machine specific 한 sched clock 초기화 함수 호출
 **/
static void __init v2m_init_early(void)
{
	/** 20130518
	 * vexpress의 경우 ct_desc에 해당하는 ct_ca9x4_desc.
	 *   init_early 함수는 지정되어 있지 않음. 
	 **/
	if (ct_desc->init_early)
		ct_desc->init_early();
	/** 20130601
	 * timer counter register의 주소와 cpu의 기준 clock을 인자로 호출
	 **/
	versatile_sched_clock_init(v2m_sysreg_base + V2M_SYS_24MHZ, 24000000);
}

/** 20151121
 **/
static void v2m_power_off(void)
{
	if (v2m_cfg_write(SYS_CFG_SHUTDOWN | SYS_CFG_SITE(SYS_CFG_SITE_MB), 0))
		printk(KERN_EMERG "Unable to shutdown\n");
}

static void v2m_restart(char str, const char *cmd)
{
	if (v2m_cfg_write(SYS_CFG_REBOOT | SYS_CFG_SITE(SYS_CFG_SITE_MB), 0))
		printk(KERN_EMERG "Unable to reboot\n");
}

struct ct_desc *ct_desc;

/** 20150613
 **/
static struct ct_desc *ct_descs[] __initdata = {
#ifdef CONFIG_ARCH_VEXPRESS_CA9X4
	&ct_ca9x4_desc,
#endif
};
/** 20130323
 *	v2m processor ID를 읽어와서 해당하는 device descriptor 를 찾아 ct_desc 에 저장한다.
 **/
static void __init v2m_populate_ct_desc(void)
{
	int i;
	u32 current_tile_id;

	ct_desc = NULL;
	current_tile_id = readl(v2m_sysreg_base + V2M_SYS_PROCID0)
				& V2M_CT_ID_MASK;

	for (i = 0; i < ARRAY_SIZE(ct_descs) && !ct_desc; ++i)
		if (ct_descs[i]->id == current_tile_id)
			ct_desc = ct_descs[i];

	if (!ct_desc)
		panic("vexpress: this kernel does not support core tile ID 0x%08x when booting via ATAGs.\n"
		      "You may need a device tree blob or a different kernel to boot on this board.\n",
		      current_tile_id);
}

/** 20130323
 * vm_list에 v2m_io_desc와 ct_desc를 단일 링크로 연결시킴
 **/
static void __init v2m_map_io(void)
{
	/** 20130323
	 *	ARRAY_SIZE 는 1이 리턴 됨. 
	 **/
	iotable_init(v2m_io_desc, ARRAY_SIZE(v2m_io_desc));
	/** 20130323
	 * v2m_sysreg_base = VA 0xF800 0000
	 **/
	v2m_sysreg_base = ioremap(V2M_SYSREGS, SZ_4K);
	v2m_populate_ct_desc();
	/** 20130330
	 *  ct_desc는 v2m_populate_ct_desc에서 찾아 채워넣은 구조체
	 *
	 *  vexpress의 ct_desc : ct_ca9x4_desc
	 *    .map_io = ct_ca9x4_map_io,
	 **/
	ct_desc->map_io();
}

/** 20140906
 * vexpress는 cpu daughter board의 init_irq를 호출한다.
 *  => ct_ca9x4_init_irq
 **/
static void __init v2m_init_irq(void)
{
	ct_desc->init_irq();
}

/** 20151121
 * v2m machine에 대한 초기화.
 *
 * - platform bus와 amba bus가 초기화된 상태이므로, 플랫폼 디바이스들을 등록한다.
 * - 플랫폼용 콜백 함수를 지정한다.
 **/
static void __init v2m_init(void)
{
	int i;

	/** 20151114
	 * ethernet supplies를 등록한다.
	 **/
	regulator_register_fixed(0, v2m_eth_supplies,
			ARRAY_SIZE(v2m_eth_supplies));

	/** 20151121
	 * 정적 구조체로 설정한 플랫폼 디바이스들을 추가한다.
	 *
	 * 플랫폼 디바이스 구조체를 동적 생성할 때는
	 * platform_device_alloc을 사용한다.
	 **/
	platform_device_register(&v2m_pcie_i2c_device);
	platform_device_register(&v2m_ddc_i2c_device);
	platform_device_register(&v2m_flash_device);
	platform_device_register(&v2m_cf_device);
	platform_device_register(&v2m_eth_device);
	platform_device_register(&v2m_usb_device);

	/** 20151121
	 * v2m의 amba 디바이스들을 등록한다.
	 *
	 * 리소스는 (버스이므로) iomem_resource 아래에 추가된다.
	 **/
	for (i = 0; i < ARRAY_SIZE(v2m_amba_devs); i++)
		amba_device_register(v2m_amba_devs[i], &iomem_resource);

	pm_power_off = v2m_power_off;

	/** 20140920
	 * ct_ca9x4_init 호출.
	 **/
	ct_desc->init_tile();
}

/** 20140830
 * vexpress용 MACHINE 정의.
 *
 * timer :      v2m_timer
 * handle_irq:  gic_handle_irq
 **/
MACHINE_START(VEXPRESS, "ARM-Versatile Express")
	.atag_offset	= 0x100,
	.map_io		= v2m_map_io,
	.init_early	= v2m_init_early,
	.init_irq	= v2m_init_irq,
	.timer		= &v2m_timer,
	.handle_irq	= gic_handle_irq,
	.init_machine	= v2m_init,
	.restart	= v2m_restart,
MACHINE_END

#if defined(CONFIG_ARCH_VEXPRESS_DT)

static struct map_desc v2m_rs1_io_desc __initdata = {
	.virtual	= V2M_PERIPH,
	.pfn		= __phys_to_pfn(0x1c000000),
	.length		= SZ_2M,
	.type		= MT_DEVICE,
};

static int __init v2m_dt_scan_memory_map(unsigned long node, const char *uname,
		int depth, void *data)
{
	const char **map = data;

	if (strcmp(uname, "motherboard") != 0)
		return 0;

	*map = of_get_flat_dt_prop(node, "arm,v2m-memory-map", NULL);

	return 1;
}

void __init v2m_dt_map_io(void)
{
	const char *map = NULL;

	of_scan_flat_dt(v2m_dt_scan_memory_map, &map);

	if (map && strcmp(map, "rs1") == 0)
		iotable_init(&v2m_rs1_io_desc, 1);
	else
		iotable_init(v2m_io_desc, ARRAY_SIZE(v2m_io_desc));

#if defined(CONFIG_SMP)
	vexpress_dt_smp_map_io();
#endif
}

void __init v2m_dt_init_early(void)
{
	struct device_node *node;
	u32 dt_hbi;

	node = of_find_compatible_node(NULL, NULL, "arm,vexpress-sysreg");
	v2m_sysreg_base = of_iomap(node, 0);
	if (WARN_ON(!v2m_sysreg_base))
		return;

	/* Confirm board type against DT property, if available */
	if (of_property_read_u32(allnodes, "arm,hbi", &dt_hbi) == 0) {
		int site = v2m_get_master_site();
		u32 id = readl(v2m_sysreg_base + (site == SYS_CFG_SITE_DB2 ?
				V2M_SYS_PROCID1 : V2M_SYS_PROCID0));
		u32 hbi = id & SYS_PROCIDx_HBI_MASK;

		if (WARN_ON(dt_hbi != hbi))
			pr_warning("vexpress: DT HBI (%x) is not matching "
					"hardware (%x)!\n", dt_hbi, hbi);
	}
}

static  struct of_device_id vexpress_irq_match[] __initdata = {
	{ .compatible = "arm,cortex-a9-gic", .data = gic_of_init, },
	{}
};

static void __init v2m_dt_init_irq(void)
{
	of_irq_init(vexpress_irq_match);
}

static void __init v2m_dt_timer_init(void)
{
	struct device_node *node;
	const char *path;
	int err;

	node = of_find_compatible_node(NULL, NULL, "arm,sp810");
	v2m_sysctl_init(of_iomap(node, 0));

	v2m_clk_init();

	err = of_property_read_string(of_aliases, "arm,v2m_timer", &path);
	if (WARN_ON(err))
		return;
	node = of_find_node_by_path(path);
	v2m_sp804_init(of_iomap(node, 0), irq_of_parse_and_map(node, 0));
	if (arch_timer_of_register() != 0)
		twd_local_timer_of_register();

	if (arch_timer_sched_clock_init() != 0)
		versatile_sched_clock_init(v2m_sysreg_base + V2M_SYS_24MHZ, 24000000);
}

static struct sys_timer v2m_dt_timer = {
	.init = v2m_dt_timer_init,
};

static struct of_dev_auxdata v2m_dt_auxdata_lookup[] __initdata = {
	OF_DEV_AUXDATA("arm,vexpress-flash", V2M_NOR0, "physmap-flash",
			&v2m_flash_data),
	OF_DEV_AUXDATA("arm,primecell", V2M_MMCI, "mb:mmci", &v2m_mmci_data),
	/* RS1 memory map */
	OF_DEV_AUXDATA("arm,vexpress-flash", 0x08000000, "physmap-flash",
			&v2m_flash_data),
	OF_DEV_AUXDATA("arm,primecell", 0x1c050000, "mb:mmci", &v2m_mmci_data),
	{}
};

static void __init v2m_dt_init(void)
{
	l2x0_of_init(0x00400000, 0xfe0fffff);
	of_platform_populate(NULL, of_default_bus_match_table,
			v2m_dt_auxdata_lookup, NULL);
	pm_power_off = v2m_power_off;
}

const static char *v2m_dt_match[] __initconst = {
	"arm,vexpress",
	NULL,
};

DT_MACHINE_START(VEXPRESS_DT, "ARM-Versatile Express")
	.dt_compat	= v2m_dt_match,
	.map_io		= v2m_dt_map_io,
	.init_early	= v2m_dt_init_early,
	.init_irq	= v2m_dt_init_irq,
	.timer		= &v2m_dt_timer,
	.init_machine	= v2m_dt_init,
	.handle_irq	= gic_handle_irq,
	.restart	= v2m_restart,
MACHINE_END

#endif
