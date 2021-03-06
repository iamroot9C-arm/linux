/*
 * Versatile Express Core Tile Cortex A9x4 Support
 */
#include <linux/init.h>
#include <linux/gfp.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/amba/bus.h>
#include <linux/amba/clcd.h>
#include <linux/clkdev.h>

#include <asm/hardware/arm_timer.h>
#include <asm/hardware/cache-l2x0.h>
#include <asm/hardware/gic.h>
#include <asm/pmu.h>
#include <asm/smp_scu.h>
#include <asm/smp_twd.h>

#include <mach/ct-ca9x4.h>

#include <asm/hardware/timer-sp.h>

#include <asm/mach/map.h>
#include <asm/mach/time.h>

#include "core.h"

#include <mach/motherboard.h>

#include <plat/clcd.h>

static struct map_desc ct_ca9x4_io_desc[] __initdata = {
	{
		.virtual        = V2T_PERIPH,
		.pfn            = __phys_to_pfn(CT_CA9X4_MPIC),
		.length         = SZ_8K,
		.type           = MT_DEVICE,
	},
};

/** 20130330
 * ct_ca9x4의 io_desc 항목에 대해 page table에 등록하고 vmlist에 추가
 **/
static void __init ct_ca9x4_map_io(void)
{
	iotable_init(ct_ca9x4_io_desc, ARRAY_SIZE(ct_ca9x4_io_desc));
}

#ifdef CONFIG_HAVE_ARM_TWD
/** 20140913
 * TWD local timer 정의.
 * resource mem : A9_MPCORE_TWD
 * resource irq : IRQ_LOCALTIMER(29)
 **/
static DEFINE_TWD_LOCAL_TIMER(twd_local_timer, A9_MPCORE_TWD, IRQ_LOCALTIMER);

/** 20140920
 * twd_local_timer를 local timer(percpu)로 등록한다.
 **/
static void __init ca9x4_twd_init(void)
{
	int err = twd_local_timer_register(&twd_local_timer);
	if (err)
		pr_err("twd_local_timer_register failed %d\n", err);
}
#else
#define ca9x4_twd_init()	do {} while(0)
#endif

/** 20140920
 * CoreTile ca9x4의 irq를 초기화 한다.
 *   - gic 초기화
 *   - twd를 local timer로 지정(irq 29)
 **/
static void __init ct_ca9x4_init_irq(void)
{
	/** 20140906
	 * A9_MPCORE_GIC_DIST, A9_MPCORE_GIC_CPU는 Physical address.
	 * virtual address로 mapping 시켜 주소를 전달한다.
	 **/
	gic_init(0, 29, ioremap(A9_MPCORE_GIC_DIST, SZ_4K),
		 ioremap(A9_MPCORE_GIC_CPU, SZ_256));
	ca9x4_twd_init();
}

static void ct_ca9x4_clcd_enable(struct clcd_fb *fb)
{
	u32 site = v2m_get_master_site();

	/*
	 * Old firmware was using the "site" component of the command
	 * to control the DVI muxer (while it should be always 0 ie. MB).
	 * Newer firmware uses the data register. Keep both for compatibility.
	 */
	v2m_cfg_write(SYS_CFG_MUXFPGA | SYS_CFG_SITE(site), site);
	v2m_cfg_write(SYS_CFG_DVIMODE | SYS_CFG_SITE(SYS_CFG_SITE_MB), 2);
}

static int ct_ca9x4_clcd_setup(struct clcd_fb *fb)
{
	unsigned long framesize = 1024 * 768 * 2;

	fb->panel = versatile_clcd_get_panel("XVGA");
	if (!fb->panel)
		return -EINVAL;

	return versatile_clcd_setup_dma(fb, framesize);
}

static struct clcd_board ct_ca9x4_clcd_data = {
	.name		= "CT-CA9X4",
	.caps		= CLCD_CAP_5551 | CLCD_CAP_565,
	.check		= clcdfb_check,
	.decode		= clcdfb_decode,
	.enable		= ct_ca9x4_clcd_enable,
	.setup		= ct_ca9x4_clcd_setup,
	.mmap		= versatile_clcd_mmap_dma,
	.remove		= versatile_clcd_remove_dma,
};

static AMBA_AHB_DEVICE(clcd, "ct:clcd", 0, CT_CA9X4_CLCDC, IRQ_CT_CA9X4_CLCDC, &ct_ca9x4_clcd_data);
static AMBA_APB_DEVICE(dmc, "ct:dmc", 0, CT_CA9X4_DMC, IRQ_CT_CA9X4_DMC, NULL);
static AMBA_APB_DEVICE(smc, "ct:smc", 0, CT_CA9X4_SMC, IRQ_CT_CA9X4_SMC, NULL);
static AMBA_APB_DEVICE(gpio, "ct:gpio", 0, CT_CA9X4_GPIO, IRQ_CT_CA9X4_GPIO, NULL);

/** 20151121
 * AMBA_AHB_DEVICE, AMBA_APB_DEVICE 매크로로 선언한 cortex-a9 quad board 디바이스
 **/
static struct amba_device *ct_ca9x4_amba_devs[] __initdata = {
	&clcd_device,
	&dmc_device,
	&smc_device,
	&gpio_device,
};


static struct v2m_osc ct_osc1 = {
	.osc = 1,
	.rate_min = 10000000,
	.rate_max = 80000000,
	.rate_default = 23750000,
};

static struct resource pmu_resources[] = {
	[0] = {
		.start	= IRQ_CT_CA9X4_PMU_CPU0,
		.end	= IRQ_CT_CA9X4_PMU_CPU0,
		.flags	= IORESOURCE_IRQ,
	},
	[1] = {
		.start	= IRQ_CT_CA9X4_PMU_CPU1,
		.end	= IRQ_CT_CA9X4_PMU_CPU1,
		.flags	= IORESOURCE_IRQ,
	},
	[2] = {
		.start	= IRQ_CT_CA9X4_PMU_CPU2,
		.end	= IRQ_CT_CA9X4_PMU_CPU2,
		.flags	= IORESOURCE_IRQ,
	},
	[3] = {
		.start	= IRQ_CT_CA9X4_PMU_CPU3,
		.end	= IRQ_CT_CA9X4_PMU_CPU3,
		.flags	= IORESOURCE_IRQ,
	},
};

/** 20151121
 * ARM Performance Monitor Unit 디바이스 선언 
 **/
static struct platform_device pmu_device = {
	.name		= "arm-pmu",
	.id		= ARM_PMU_DEVICE_CPU,
	.num_resources	= ARRAY_SIZE(pmu_resources),
	.resource	= pmu_resources,
};

/** 20151121
 * CoreTile cortex-a9x4 관련 초기화를 수행한다.
 *
 * - L2 cache register를 페이지 테이블에 매핑하고 초기화.
 * - 클럭 디바이스를 등록
 * - ca9x4의 amba 디바이스들을 등록.
 *
 **/
static void __init ct_ca9x4_init(void)
{
	int i;
	struct clk *clk;

#ifdef CONFIG_CACHE_L2X0
	/** 20140920
	 * L2C register를 page table에 mapping 한다.
	 **/
	void __iomem *l2x0_base = ioremap(CT_CA9X4_L2CC, SZ_4K);

	/* set RAM latencies to 1 cycle for this core tile. */
	writel(0, l2x0_base + L2X0_TAG_LATENCY_CTRL);
	writel(0, l2x0_base + L2X0_DATA_LATENCY_CTRL);

	l2x0_init(l2x0_base, 0x00400000, 0xfe0fffff);
#endif

	ct_osc1.site = v2m_get_master_site();
	clk = v2m_osc_register("ct:osc1", &ct_osc1);
	clk_register_clkdev(clk, NULL, "ct:clcd");

	for (i = 0; i < ARRAY_SIZE(ct_ca9x4_amba_devs); i++)
		amba_device_register(ct_ca9x4_amba_devs[i], &iomem_resource);

	/** 20151121
	 * arm pmu를 플랫폼 버스에 추가.
	 **/
	platform_device_register(&pmu_device);
}

#ifdef CONFIG_SMP
static void *ct_ca9x4_scu_base __initdata;

/** 20130518
 * SMP core 관련 초기화
 *     vexpress의 경우 CPU board가 Cortex-A9 MPcore로 존재. 관련 내용은 Cortex-A9 MPcore TRM 참고.
 * 
 * - SMP MPcore SCU를 page table mapping.
 * - SCU에서 cpu 개수 정보를 읽어와 possible bitmap table을 설정.
 * - smp_cross_call로 gic_raise_softirq 지정 (GIC를 통해 IPI 전달)
 **/
static void __init ct_ca9x4_init_cpu_map(void)
{
	int i, ncores;

	/** 20130518
	 * ioremap으로 가상 주소를 리턴. vm_area_add_early에서 vmlist에 등록했음.
	 *   NULL인 경우 WARN을 출력하고 리턴.
	 **/
	ct_ca9x4_scu_base = ioremap(A9_MPCORE_SCU, SZ_128);
	if (WARN_ON(!ct_ca9x4_scu_base))
		return;

	/** 20130518
	 * ncores는 SCU CONFIG register에서 읽어온 값 + 1
	 * (Number of CPUs present in the Cortex-A9 MPCore processor)
	 **/
	ncores = scu_get_core_count(ct_ca9x4_scu_base);

	/** 20130518
	 * 커널 설정값보다 ncores가 크면 최대값을 커널 설정값인 nr_cpu_ids로 잡아줌.
	 **/
	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	/** 20130518
	 * ncores 개수를 순회하며 cpu에 해당하는 비트를 possible로 설정.
	 **/
	for (i = 0; i < ncores; ++i)
		set_cpu_possible(i, true);

	/** 20130518
	 * gic_raise_softirq 를 smp_cross_call로 등록
	 **/
	set_smp_cross_call(gic_raise_softirq);
}

/** 20150118
 * SCU enable.
 **/
static void __init ct_ca9x4_smp_enable(unsigned int max_cpus)
{
	scu_enable(ct_ca9x4_scu_base);
}
#endif

/** 20140906
 * coretile cortex-a9 quad용 descriptor.
 **/
struct ct_desc ct_ca9x4_desc __initdata = {
	.id		= V2M_CT_ID_CA9,
	.name		= "CA9x4",
	.map_io		= ct_ca9x4_map_io,
	.init_irq	= ct_ca9x4_init_irq,
	.init_tile	= ct_ca9x4_init,
#ifdef CONFIG_SMP
	/** 20130518
	 * init_cpu_map : smp_init_cpus 에서 호출
	 * 20150118
	 * smp_enable : platform_smp_prepare_cpus 에서 호출
	 **/
	.init_cpu_map	= ct_ca9x4_init_cpu_map,
	.smp_enable	= ct_ca9x4_smp_enable,
#endif
};
