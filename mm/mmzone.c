/*
 * linux/mm/mmzone.c
 *
 * management codes for pgdats and zones.
 */


#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>

/** 20140621
 * UMA 구조에서는 &contig_page_data가 리턴된다.
 **/
struct pglist_data *first_online_pgdat(void)
{
	return NODE_DATA(first_online_node);
}

struct pglist_data *next_online_pgdat(struct pglist_data *pgdat)
{
	int nid = next_online_node(pgdat->node_id);

	if (nid == MAX_NUMNODES)
		return NULL;
	return NODE_DATA(nid);
}

/*
 * next_zone - helper magic for for_each_zone()
 */
struct zone *next_zone(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		pgdat = next_online_pgdat(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

/** 20130727
 * UMA일 경우 항상 1리턴.
 * NUMA일 경우 nodemask에 node index가 포함되는지 검사해 리턴
 **/
static inline int zref_in_nodemask(struct zoneref *zref, nodemask_t *nodes)
{
#ifdef CONFIG_NUMA
	return node_isset(zonelist_node_idx(zref), *nodes);
#else
	return 1;
#endif /* CONFIG_NUMA */
}

/* Returns the next zone at or below highest_zoneidx in a zonelist */
struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes,
					struct zone **zone)
{
	/*
	 * Find the next suitable zone to use for the allocation.
	 * Only filter based on nodemask if it's set
	 */
	/** 20130727
	 * nodemask가 지정되지 않았을 때
	 **/
	if (likely(nodes == NULL))
		/** 20130727
		 * 지정된 highest_zoneidx (zone type)을 넘는다면 다음 zone으로 넘어간다.
		 **/
		while (zonelist_zone_idx(z) > highest_zoneidx)
			z++;
	/** 20130727
	 * nodemask가 지정되어 있을 때
	 **/
	else
		/** 20130727
		 * 지정된 highest_zoneidx (zone type)을 넘거나
		 * z에 해당하는 노드가 nodemask에 포함되어 있지 않다면
		 *   다음 zone으로 넘어간다.
		 **/
		while (zonelist_zone_idx(z) > highest_zoneidx ||
				(z->zone && !zref_in_nodemask(z, nodes)))
			z++;

	*zone = zonelist_zone(z);
	return z;
}

#ifdef CONFIG_ARCH_HAS_HOLES_MEMORYMODEL
int memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone)
{
	if (page_to_pfn(page) != pfn)
		return 0;

	if (page_zone(page) != zone)
		return 0;

	return 1;
}
#endif /* CONFIG_ARCH_HAS_HOLES_MEMORYMODEL */

/** 20130427
 * lruvec 자료구조 초기화
 **/
void lruvec_init(struct lruvec *lruvec, struct zone *zone)
{
	enum lru_list lru;

	/** 20130427
	 * lruvec 자료구조 초기화
	 **/
	memset(lruvec, 0, sizeof(struct lruvec));

	/** 20130427
	 * 각 list head를 초기화
	 **/
	for_each_lru(lru)
		INIT_LIST_HEAD(&lruvec->lists[lru]);

	/** 20130427
	 * vexpress 에서는 0
	 **/
#ifdef CONFIG_MEMCG
	lruvec->zone = zone;
#endif
}
