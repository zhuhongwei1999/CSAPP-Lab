#include "common.h"
#include <time.h>

typedef struct {
	struct {
		uint8_t valid : 1; // 有效位
		uint8_t dirty : 1; // 脏位
	} flag;		//使用结构体位域，分出两个高位为有效位和脏位
	uint32_t tag; // 标记
	uint8_t data[BLOCK_SIZE]; // 数据块64个字节
} CacheRow;

int row_num;	//行数
int group_num;	// 组数
int group_id_width;	// 组号部分的长度
int tag_width;		// tag部分的长度
CacheRow **cache;	// cache，二维数组，第一维为行数，第二维为每一行的项（包括数据，Tag等等）


void mem_read(uintptr_t block_num, uint8_t *buf);
void mem_write(uintptr_t block_num, const uint8_t *buf);
uint32_t block_read(CacheRow *row, uint32_t block_offset);
uint32_t replace(uint32_t block_num, uint32_t set_id);
void block_write(CacheRow *row, uint32_t block_offset, uint32_t data, uint32_t wmask);


// 从cache中读出`addr`地址处的4字节数据
// 若缺失, 需要先从内存中读入数据
uint32_t cache_read(uintptr_t addr) {
	try_increase(1);
	int r;
    // 将cache每一行分成块内地址、tag、组号。通过位操作来实现
	uint32_t addr_temp = addr & ~0x3;
	uint32_t block_offset = addr_temp & ~(~0 << BLOCK_WIDTH);
	uint32_t set_id = (addr_temp >> 6) & ~(~0 << group_id_width);
	uint32_t tag = (addr_temp >> (6 + group_id_width)) & ~(~0 << tag_width);
	// 寻找是否匹配
	for (int i = 0; i < row_num; i++) {
		if (cache[set_id][i].flag.valid == 0) continue;
		if (cache[set_id][i].tag == tag) {
			hit_increase(1);
			return block_read(&cache[set_id][i], block_offset);
		}
	}
	//如果匹配失败，则从内存中读取相应的块，并对相应cache组进行随机替换
	r = replace(addr_temp >> BLOCK_WIDTH, set_id);
	return block_read(&cache[set_id][r], block_offset);
}

uint32_t block_read(CacheRow *row, uint32_t block_offset) {
	union {
		uint8_t data[4];
		uint32_t val;
	} temp;	//将32位的数据分成4组，每组8位
	for (int i = 0; i < 4; i++) {
		temp.data[i] = (*row).data[block_offset + i];
	}
	return temp.val;
}


// 往cache中`addr`地址所属的块写入数据`data`, 写掩码为`wmask`
// 例如当`wmask`为`0xff`时, 只写入低8比特
// 若缺失, 需要从先内存中读入数据
void cache_write(uintptr_t addr, uint32_t data, uint32_t wmask) {
	try_increase(1);
	int r;
	// 解析出标记、组号和块内偏移
	uint32_t addr_temp = addr & ~0x3;
	uint32_t block_offset = addr_temp & ~(~0 << BLOCK_WIDTH);
	uint32_t set_id = (addr_temp >> 6) & ~(~0 << group_id_width);
	uint32_t tag = (addr_temp >> (6 + group_id_width)) & ~(~0 << tag_width);
	// 从对应组中找寻是否存在匹配的cache行
	for (int i = 0; i < row_num; i++) {
		if (cache[set_id][i].flag.valid == 0)
			continue;
		if (cache[set_id][i].tag == tag) {
			hit_increase(1);
			cache[set_id][i].flag.dirty = 1; // 设置脏位
			block_write(&cache[set_id][i], block_offset, data, wmask);
			return;
		}
	}
	// 如果匹配失败，则进行写分配 
	r = replace(addr_temp >> BLOCK_WIDTH, set_id);
	cache[set_id][r].flag.dirty = 1; // 设置脏位
	block_write(&cache[set_id][r], block_offset, data, wmask);
	return;
}

void block_write(CacheRow *row, uint32_t block_offset, uint32_t data, uint32_t wmask) {
	union {
		uint8_t data[4];
		uint32_t val;
	} temp;
	temp.val = block_read(row, block_offset);
	temp.val = (temp.val & ~wmask) | (data & wmask);
	for (int i = 0; i < 4; i++) {
		(*row).data[block_offset + i] = temp.data[i];
	}
}


uint32_t replace(uint32_t block_num, uint32_t set_id) {
    // 随机生成行号。
	srand(time(0));
	int row = rand() % row_num;
    //若脏位为1，则将cache中的数据写回内存
	if (cache[set_id][row].flag.dirty == 1) {
		mem_write((cache[set_id][row].tag << group_id_width) + set_id, cache[set_id][row].data);
	}
	mem_read(block_num, cache[set_id][row].data);
	cache[set_id][row].flag.valid = 1;
	cache[set_id][row].flag.dirty = 0;
	cache[set_id][row].tag = block_num >> group_id_width;
	return row;
}

void init_cache(int total_size_width, int associativity_width) {
	row_num = exp2(associativity_width);
	group_num = exp2(total_size_width) / row_num / BLOCK_SIZE; 
	group_id_width = total_size_width - BLOCK_WIDTH - associativity_width;
	tag_width = 32 - BLOCK_WIDTH - group_id_width;
	cache = (CacheRow**)malloc(sizeof(CacheRow*) * group_num);
	for (int i = 0; i < group_num; i++) {
		cache[i] = (CacheRow*)malloc(sizeof(CacheRow) * row_num);
		for (int j = 0; j < row_num; j++) {
			cache[i][j].flag.valid = 0;
			cache[i][j].flag.dirty = 0;
			cache[i][j].tag = 0;
		}
	}
}