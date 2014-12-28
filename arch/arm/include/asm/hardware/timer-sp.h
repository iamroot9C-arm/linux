void __sp804_clocksource_and_sched_clock_init(void __iomem *,
					      const char *, int);

/** 20141227    
 * name을 가지는 sp804 장치를 clocksource로 사용하기 위해 초기화 하고 구조체를 등록한다.
 * sched_clock으로는 설정하지 않는다.
 **/
static inline void sp804_clocksource_init(void __iomem *base, const char *name)
{
	__sp804_clocksource_and_sched_clock_init(base, name, 0);
}

static inline void sp804_clocksource_and_sched_clock_init(void __iomem *base,
							  const char *name)
{
	__sp804_clocksource_and_sched_clock_init(base, name, 1);
}

void sp804_clockevents_init(void __iomem *, unsigned int, const char *);
