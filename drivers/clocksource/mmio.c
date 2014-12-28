/*
 * Generic MMIO clocksource support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clocksource.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>

struct clocksource_mmio {
	void __iomem *reg;
	struct clocksource clksrc;
};

static inline struct clocksource_mmio *to_mmio_clksrc(struct clocksource *c)
{
	return container_of(c, struct clocksource_mmio, clksrc);
}

cycle_t clocksource_mmio_readl_up(struct clocksource *c)
{
	return readl_relaxed(to_mmio_clksrc(c)->reg);
}

/** 20141227    
 **/
cycle_t clocksource_mmio_readl_down(struct clocksource *c)
{
	return ~readl_relaxed(to_mmio_clksrc(c)->reg);
}

cycle_t clocksource_mmio_readw_up(struct clocksource *c)
{
	return readw_relaxed(to_mmio_clksrc(c)->reg);
}

cycle_t clocksource_mmio_readw_down(struct clocksource *c)
{
	return ~(unsigned)readw_relaxed(to_mmio_clksrc(c)->reg);
}

/**
 * clocksource_mmio_init - Initialize a simple mmio based clocksource
 * @base:	Virtual address of the clock readout register
 * @name:	Name of the clocksource
 * @hz:		Frequency of the clocksource in Hz
 * @rating:	Rating of the clocksource
 * @bits:	Number of valid bits
 * @read:	One of clocksource_mmio_read*() above
 */
/** 20141227    
 * mmio로 mapping된 clocksource에 대한
 * clocksource 구조체를 설정하고 등록한다.
 **/
int __init clocksource_mmio_init(void __iomem *base, const char *name,
	unsigned long hz, int rating, unsigned bits,
	cycle_t (*read)(struct clocksource *))
{
	struct clocksource_mmio *cs;

	if (bits > 32 || bits < 16)
		return -EINVAL;

	/** 20141227    
	 * clocksource_mmio를 위한 메모리 공간을 할당 받아, 
	 * 전달받은 매개변수로 설정한다.
	 **/
	cs = kzalloc(sizeof(struct clocksource_mmio), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;

	cs->reg = base;
	cs->clksrc.name = name;
	cs->clksrc.rating = rating;
	cs->clksrc.read = read;
	cs->clksrc.mask = CLOCKSOURCE_MASK(bits);
	cs->clksrc.flags = CLOCK_SOURCE_IS_CONTINUOUS;

	/** 20141227    
	 * 생성한 clocksource 정보와 hz(rate)를 전달해
	 * clocksource를 rating 순으로 list에 등록한다.
	 **/
	return clocksource_register_hz(&cs->clksrc, hz);
}
