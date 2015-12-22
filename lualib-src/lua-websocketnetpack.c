#include "skynet_malloc.h"
#include "skynet_socket.h"

#include <lua.h>
#include <lauxlib.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>

#define QUEUESIZE 1024
#define HASHSIZE 4096
#define SMALLSTRING 2048
#define HEADERSIZE 1024
#define WEBSOCKET_HEADER_LEN  2
#define WEBSOCKET_MASK_LEN    4

#define TYPE_DATA 1
#define TYPE_MORE 2
#define TYPE_ERROR 3
#define TYPE_OPEN 4
#define TYPE_CLOSE 5
#define TYPE_WARNING 6

struct netpack {
	int id;
	int size;
	void * buffer;
};

struct uncomplete {
	struct netpack pack;
	struct uncomplete * next;
	uint8_t header[HEADERSIZE];
	int header_size;
	int read;
    // websocket mask
    int mask;
    int ismask;
    int hasunmask_size;
};

struct queue {
	int cap;
	int head;
	int tail;
	struct uncomplete * hash[HASHSIZE];
	struct netpack queue[QUEUESIZE];
};

static void
clear_list(struct uncomplete * uc) {
	while (uc) {
		skynet_free(uc->pack.buffer);
		void * tmp = uc;
		uc = uc->next;
		skynet_free(tmp);
	}
}

static int
lclear(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL) {
		return 0;
	}
	int i;
	for (i=0;i<HASHSIZE;i++) {
		clear_list(q->hash[i]);
		q->hash[i] = NULL;
	}
	if (q->head > q->tail) {
		q->tail += q->cap;
	}
	for (i=q->head;i<q->tail;i++) {
		struct netpack *np = &q->queue[i % q->cap];
		skynet_free(np->buffer);
	}
	q->head = q->tail = 0;

	return 0;
}

static inline int
hash_fd(int fd) {
	int a = fd >> 24;
	int b = fd >> 12;
	int c = fd;
	return (int)(((uint32_t)(a + b + c)) % HASHSIZE);
}

static struct uncomplete *
find_uncomplete(struct queue *q, int fd) {
	if (q == NULL)
		return NULL;
	int h = hash_fd(fd);
	struct uncomplete * uc = q->hash[h];
	if (uc == NULL)
		return NULL;
	if (uc->pack.id == fd) {
		q->hash[h] = uc->next;
		return uc;
	}
	struct uncomplete * last = uc;
	while (last->next) {
		uc = last->next;
		if (uc->pack.id == fd) {
			last->next = uc->next;
			return uc;
		}
		last = uc;
	}
	return NULL;
}

static struct queue *
get_queue(lua_State *L) {
	struct queue *q = lua_touserdata(L,1);
	if (q == NULL) {
		q = lua_newuserdata(L, sizeof(struct queue));
		q->cap = QUEUESIZE;
		q->head = 0;
		q->tail = 0;
		int i;
		for (i=0;i<HASHSIZE;i++) {
			q->hash[i] = NULL;
		}
		lua_replace(L, 1);
	}
	return q;
}

static void
expand_queue(lua_State *L, struct queue *q) {
	struct queue *nq = lua_newuserdata(L, sizeof(struct queue) + q->cap * sizeof(struct netpack));
	nq->cap = q->cap + QUEUESIZE;
	nq->head = 0;
	nq->tail = q->cap;
	memcpy(nq->hash, q->hash, sizeof(nq->hash));
	memset(q->hash, 0, sizeof(q->hash));
	int i;
	for (i=0;i<q->cap;i++) {
		int idx = (q->head + i) % q->cap;
		nq->queue[i] = q->queue[idx];
	}
	q->head = q->tail = 0;
	lua_replace(L,1);
}

static void
push_data(lua_State *L, int fd, void *buffer, int size, int clone) {
	if (clone) {
		void * tmp = skynet_malloc(size);
		memcpy(tmp, buffer, size);
		buffer = tmp;
	}
	struct queue *q = get_queue(L);
	struct netpack *np = &q->queue[q->tail];
	if (++q->tail >= q->cap)
		q->tail -= q->cap;
	np->id = fd;
	np->buffer = buffer;
	np->size = size;
	if (q->head == q->tail) {
		expand_queue(L, q);
	}
}

static struct uncomplete *
save_uncomplete(lua_State *L, int fd) {
	struct queue *q = get_queue(L);
	int h = hash_fd(fd);
	struct uncomplete * uc = skynet_malloc(sizeof(struct uncomplete));
	memset(uc, 0, sizeof(*uc));
	uc->next = q->hash[h];
	uc->pack.id = fd;
	q->hash[h] = uc;

	return uc;
}

static uint64_t ntoh64(uint64_t host)
{
    uint64_t ret = 0;
    uint32_t high, low;
    low = host & 0xFFFFFFFF;
    high = (host >> 32) & 0xFFFFFFFF;
    low = ntohl(low);
    high = ntohl(high);
    ret = low;
    ret <<= 32;
    ret |= high;
    return ret;
}
/*
* @return -1表示包头长不够 -2表示包前两个字节无效逻辑需要扔掉
*/
static inline int
read_size(uint8_t * buffer, int size, int* pack_head_length, int* mask, int * ismask, int * hasunmask_size) {
	
	if (size < 2)
	{
		return -1;
	}
	
	//char fin = (buffer[0] >> 7) & 0x1;
    char rsv1 = (buffer[0] >> 6) & 0x1;
    char rsv2 = (buffer[0] >> 5) & 0x1;
    char rsv3 = (buffer[0] >> 4) & 0x1;
    //char opcode = buffer[0] & 0xf;
    char is_mask = (buffer[1] >> 7) & 0x1;


    if (0x0 != rsv1 || 0x0 != rsv2 || 0x0 != rsv3)
    {
        return -2;
    }

    int offset = 0;
	int pack_size = 0;
    //0-125
    char length = buffer[1] & 0x7f;
    offset += WEBSOCKET_HEADER_LEN;
    //126
    if (length < 0x7E)
    {
        pack_size = length;
    }
    //Short
    else if (0x7E == length)
    {
		if (size < WEBSOCKET_HEADER_LEN + sizeof(short))
		{
			return -1;
		}
        pack_size = ntohs(*((uint16_t *) (buffer+WEBSOCKET_HEADER_LEN)));
        offset += sizeof(short);
    }
    else
    {
		if (size < WEBSOCKET_HEADER_LEN + sizeof(int64_t))
		{
			return -1;
		}
        pack_size = ntoh64(*((uint64_t *) (buffer+WEBSOCKET_HEADER_LEN)));
        offset += sizeof(int64_t);
    }
	
	
    if (is_mask)
    {
        if (offset + WEBSOCKET_MASK_LEN > size)
        {
            return -1;
        }

        *ismask = 1;

        char *masks = (char*)mask;
        memcpy(masks, (buffer + offset), WEBSOCKET_MASK_LEN);
        offset += WEBSOCKET_MASK_LEN;

        int body_size = size - offset;
        int unmask_size = (pack_size > body_size) ? body_size : pack_size;
        if (unmask_size)
        {
            int i;
            for (i = 0; i < unmask_size; i++)
            {
                buffer[i + offset] ^= masks[i % WEBSOCKET_MASK_LEN];
            }
            *hasunmask_size = unmask_size;
        }
    }

	*pack_head_length = offset;

	return pack_size;
}

static void decode_wsmask_data(uint8_t* buffer, int size, struct uncomplete *uc)
{
        if (uc == NULL)
        {
            return;
        }

        if (! uc->ismask ) 
        {
            return;
        }

        char *masks = (char*)(&(uc->mask));
        if (size)
        {
            int i;
            for (i = 0; i < size; i++)
            {
                buffer[i] ^= masks[(i+uc->hasunmask_size) % WEBSOCKET_MASK_LEN];
            }
            uc->hasunmask_size += size;
        }
}

static int websocket_strnpos(char *haystack, uint32_t haystack_length, char *needle, uint32_t needle_length)
{
    assert(needle_length > 0);
    uint32_t i;

    for (i = 0; i < (int) (haystack_length - needle_length + 1); i++)
    {
        if ((haystack[0] == needle[0]) && (0 == memcmp(haystack, needle, needle_length)))
        {
            return i;
        }
        haystack++;
    }

    return -1;
}
/*
* @param  buffer   数据buffer
* @param  size     数据buffer的大小
* @return  -1表示解析失败， >0表示解析成功返回header的长度
*/
static int get_http_header(uint8_t* buffer, int size)
{
	int n = websocket_strnpos((char*)buffer, size, "\r\n\r\n", 4);
	if (n < 0)
	{
		return n;
	}
	
	return (n+4);
}
static void
push_more(lua_State *L, int fd, uint8_t *buffer, int size, int wsocket_handeshake) {

	int pack_size = 0;
	int pack_head_length = 0;
    int mask = 0;
    int ismask = 0;
    int hasunmask_size = 0;
	if (wsocket_handeshake)
	{
		//认为socket初次建立连接读取握手协议
		pack_size = get_http_header(buffer, size);			
	}
	else
	{
		//读取帧大小
		while ((pack_size = read_size(buffer, size, &pack_head_length, &mask, &ismask, &hasunmask_size)) == -2)
		{
            mask = 0;
            ismask = 0;
            hasunmask_size = 0;
			buffer += WEBSOCKET_HEADER_LEN;
			size -= WEBSOCKET_HEADER_LEN;
		}
	}
	
	if (pack_size == -1)
	{			 
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = -1;
		uc->header_size = size;
		memcpy(uc->header, buffer, size);
		return;			
	}	

	buffer += pack_head_length;
	size -= pack_head_length;

	if (size < pack_size && !wsocket_handeshake) {
		struct uncomplete * uc = save_uncomplete(L, fd);
		uc->read = size;
        uc->mask = mask;
        uc->ismask = ismask;
        uc->hasunmask_size = hasunmask_size;
		uc->pack.size = pack_size;
		uc->pack.buffer = skynet_malloc(pack_size);
		memcpy(uc->pack.buffer, buffer, size);
		return;
	}

	push_data(L, fd, buffer, pack_size, 1);

	buffer += pack_size;
	size -= pack_size;
	if (size > 0) {
		push_more(L, fd, buffer, size, wsocket_handeshake);
	}
}

static void
close_uncomplete(lua_State *L, int fd) {
	struct queue *q = lua_touserdata(L,1);
	struct uncomplete * uc = find_uncomplete(q, fd);
	if (uc) {
		skynet_free(uc->pack.buffer);
		skynet_free(uc);
	}
}

static int
filter_data_(lua_State *L, int fd, uint8_t * buffer, int size, int wsocket_handeshake) {
	struct queue *q = lua_touserdata(L,1);
	struct uncomplete * uc = find_uncomplete(q, fd);
    int pack_size = 0;
	int pack_head_length = 0;
    int mask = 0;
    int ismask = 0;
    int hasunmask_size = 0;
	if (uc) {
		// fill uncomplete
		if (uc->read < 0) {
			// read size
            int last_header_size = uc->header_size;
			int copy_size = uc->header_size + size > HEADERSIZE ? (HEADERSIZE - uc->header_size) : size;
			memcpy(uc->header+uc->header_size, buffer, copy_size);
			uc->header_size += copy_size;

			
			if (wsocket_handeshake)
			{
				//认为socket初次建立连接读取握手协议
				pack_size = get_http_header(uc->header, uc->header_size);			
			}
			else
			{
				//读取帧大小
				while ((pack_size = read_size(uc->header, uc->header_size, &pack_head_length, &mask, &ismask, &hasunmask_size)) == -2)
				{
                    mask  = 0;
                    ismask = 0;
                    hasunmask_size = 0;
					uc->header_size -= WEBSOCKET_HEADER_LEN;
					memmove(uc->header, uc->header + WEBSOCKET_HEADER_LEN, uc->header_size);
				}
			}			
			if (pack_size == -1)
			{			
				int h = hash_fd(fd);
				uc->next = q->hash[h];
				q->hash[h] = uc;
				return 1;			
			}
			//去掉帧数据的包头
			if (!wsocket_handeshake)
			{
			    uc->header_size -= pack_head_length;
				if (uc->header_size != 0)
                {
                    memmove(uc->header, uc->header + pack_head_length, uc->header_size);				
                }
			}
			
			//取得包头长度以后开始生成新包
			uc->pack.buffer = skynet_malloc(pack_size);
			uc->pack.size = pack_size;
			uc->read = uc->header_size < pack_size ? uc->header_size : pack_size;
			memcpy(uc->pack.buffer, uc->header, uc->read);
			uc->header_size -= uc->read;

            buffer += (uc->read+pack_head_length - last_header_size);
            size -= (uc->read+pack_head_length - last_header_size);
		}
		int need = uc->pack.size - uc->read;
		if (size < need) {
            decode_wsmask_data(buffer, size, uc);
			memcpy(uc->pack.buffer + uc->read, buffer, size);
			uc->read += size;
			int h = hash_fd(fd);
			uc->next = q->hash[h];
			q->hash[h] = uc;
			return 1;
		}
		if (need != 0)
		{
            decode_wsmask_data(buffer, need, uc);
			memcpy(uc->pack.buffer + uc->read, buffer, need);			
		}
		
		buffer += need;
		size -= need;
		
		if (size == 0) {
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));
			lua_pushinteger(L, fd);
			lua_pushlightuserdata(L, uc->pack.buffer);
			lua_pushinteger(L, uc->pack.size);
			skynet_free(uc);
			return 5;
		}
		// more data
		push_data(L, fd, uc->pack.buffer, uc->pack.size, 0);
		skynet_free(uc);
		push_more(L, fd, buffer, size, wsocket_handeshake);
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));
		return 2;
	} else {
		if (wsocket_handeshake)
		{
			//认为socket初次建立连接读取握手协议
			pack_size = get_http_header(buffer, size);			
		}
		else
		{
			//读取帧大小
			while ((pack_size = read_size(buffer, size, &pack_head_length, &mask, &ismask, &hasunmask_size)) == -2)
			{
                mask = 0;
                ismask = 0;
                hasunmask_size = 0;
				buffer += WEBSOCKET_HEADER_LEN;
				size -= WEBSOCKET_HEADER_LEN;
			}
		}
		
		if (pack_size == -1)
		{		
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = -1;
            uc->mask = mask;
            uc->ismask = ismask;
            uc->hasunmask_size = hasunmask_size;
			uc->header_size += size;
			memcpy(uc->header, buffer, size);
			return 1;			
		}
		buffer+=pack_head_length;
		size-=pack_head_length;
		
		if (size < pack_size && !wsocket_handeshake) {
			struct uncomplete * uc = save_uncomplete(L, fd);
			uc->read = size;
            uc->mask = mask;
            uc->ismask = ismask;
            uc->hasunmask_size = hasunmask_size;
			uc->pack.size = pack_size;
			uc->pack.buffer = skynet_malloc(pack_size);
			memcpy(uc->pack.buffer, buffer, size);
			return 1;
		}
		if (size == pack_size) {
			// just one package
			lua_pushvalue(L, lua_upvalueindex(TYPE_DATA));
			lua_pushinteger(L, fd);
			void * result = skynet_malloc(pack_size);
			memcpy(result, buffer, size);
			lua_pushlightuserdata(L, result);
			lua_pushinteger(L, size);
			return 5;
		}
		// more data
		push_data(L, fd, buffer, pack_size, 1);
		buffer += pack_size;
		size -= pack_size;
		push_more(L, fd, buffer, size, wsocket_handeshake);
		lua_pushvalue(L, lua_upvalueindex(TYPE_MORE));
		return 2;
	}
}

static inline int
filter_data(lua_State *L, int fd, uint8_t * buffer, int size, int wsocket_handeshake) {
	int ret = filter_data_(L, fd, buffer, size, wsocket_handeshake);
	// buffer is the data of socket message, it malloc at socket_server.c : function forward_message .
	// it should be free before return,
	skynet_free(buffer);
	return ret;
}

static void
pushstring(lua_State *L, const char * msg, int size) {
	if (msg) {
		lua_pushlstring(L, msg, size);
	} else {
		lua_pushliteral(L, "");
	}
}

/*
	userdata queue
	lightuserdata msg
	integer size
	return
		userdata queue
		integer type
		integer fd
		string msg | lightuserdata/integer
 */
static int
lfilter(lua_State *L) {
	struct skynet_socket_message *message = lua_touserdata(L,2);
	int size = luaL_checkinteger(L,3);
	int wsocket_handeshake = luaL_checkinteger(L,4);
	char * buffer = message->buffer;
	if (buffer == NULL) {
		buffer = (char *)(message+1);
		size -= sizeof(*message);
	} else {
		size = -1;
	}

	lua_settop(L, 1);

	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA:
		// ignore listen id (message->id)
		assert(size == -1);	// never padding string
		return filter_data(L, message->id, (uint8_t *)buffer, message->ud, wsocket_handeshake);
	case SKYNET_SOCKET_TYPE_CONNECT:
		// ignore listen fd connect
		return 1;
	case SKYNET_SOCKET_TYPE_CLOSE:
		// no more data in fd (message->id)
		close_uncomplete(L, message->id);
		lua_pushvalue(L, lua_upvalueindex(TYPE_CLOSE));
		lua_pushinteger(L, message->id);
		return 3;
	case SKYNET_SOCKET_TYPE_ACCEPT:
		lua_pushvalue(L, lua_upvalueindex(TYPE_OPEN));
		// ignore listen id (message->id);
		lua_pushinteger(L, message->ud);
		pushstring(L, buffer, size);
		return 4;
	case SKYNET_SOCKET_TYPE_ERROR:
		// no more data in fd (message->id)
		close_uncomplete(L, message->id);
		lua_pushvalue(L, lua_upvalueindex(TYPE_ERROR));
		lua_pushinteger(L, message->id);
		pushstring(L, buffer, size);
		return 4;
	case SKYNET_SOCKET_TYPE_WARNING:
		lua_pushvalue(L, lua_upvalueindex(TYPE_WARNING));
		lua_pushinteger(L, message->id);
		lua_pushinteger(L, message->ud);
		return 4;
	default:
		// never get here
		return 1;
	}
}

/*
	userdata queue
	return
		integer fd
		lightuserdata msg
		integer size
 */
static int
lpop(lua_State *L) {
	struct queue * q = lua_touserdata(L, 1);
	if (q == NULL || q->head == q->tail)
		return 0;
	struct netpack *np = &q->queue[q->head];
	if (++q->head >= q->cap) {
		q->head = 0;
	}
	lua_pushinteger(L, np->id);
	lua_pushlightuserdata(L, np->buffer);
	lua_pushinteger(L, np->size);

	return 3;
}

/*
	string msg | lightuserdata/integer

	lightuserdata/integer
 */

static const char *
tolstring(lua_State *L, size_t *sz, int index) {
	const char * ptr;
	if (lua_isuserdata(L,index)) {
		ptr = (const char *)lua_touserdata(L,index);
		*sz = (size_t)luaL_checkinteger(L, index+1);
	} else {
		ptr = luaL_checklstring(L, index, sz);
	}
	return ptr;
}

#define FRAME_SET_FIN(BYTE) (((BYTE) & 0x01) << 7)
#define FRAME_SET_OPCODE(BYTE) ((BYTE) & 0x0F)
#define FRAME_SET_MASK(BYTE) (((BYTE) & 0x01) << 7)
#define FRAME_SET_LENGTH(X64, IDX) (unsigned char)(((X64) >> ((IDX)*8)) & 0xFF)

static int
lpack(lua_State *L) {
	size_t len;
	const char * ptr = tolstring(L, &len, 1);

	int pos = 0;
    char frame_header[16];

    frame_header[pos++] = FRAME_SET_FIN(1) | FRAME_SET_OPCODE(2);
    if (len < 126)
    {
        frame_header[pos++] = FRAME_SET_MASK(0) | FRAME_SET_LENGTH(len, 0);
    }
    else
    {
        if (len < 65536)
        {
            frame_header[pos++] = FRAME_SET_MASK(0) | 126;
        }
        else
        {
            frame_header[pos++] = FRAME_SET_MASK(0) | 127;
            frame_header[pos++] = FRAME_SET_LENGTH(len, 7);
            frame_header[pos++] = FRAME_SET_LENGTH(len, 6);
            frame_header[pos++] = FRAME_SET_LENGTH(len, 5);
            frame_header[pos++] = FRAME_SET_LENGTH(len, 4);
            frame_header[pos++] = FRAME_SET_LENGTH(len, 3);
            frame_header[pos++] = FRAME_SET_LENGTH(len, 2);
        }
        frame_header[pos++] = FRAME_SET_LENGTH(len, 1);
        frame_header[pos++] = FRAME_SET_LENGTH(len, 0);
    }
		
	uint8_t * buffer = skynet_malloc(len + pos);
	memcpy(buffer, frame_header, pos);
	memcpy(buffer+pos, ptr, len);

	lua_pushlightuserdata(L, buffer);
	lua_pushinteger(L, len + pos);

	return 2;
}

static int
ltostring(lua_State *L) {
	void * ptr = lua_touserdata(L, 1);
	int size = luaL_checkinteger(L, 2);
	if (ptr == NULL) {
		lua_pushliteral(L, "");
	} else {
		lua_pushlstring(L, (const char *)ptr, size);
		skynet_free(ptr);
	}
	return 1;
}

int
luaopen_websocketnetpack(lua_State *L) {
	luaL_checkversion(L);
    luaL_Reg l[] = {
        { "pop", lpop },
        { "pack", lpack },
        { "clear", lclear },
        { "tostring", ltostring },
        { NULL, NULL },
    };
    luaL_newlib(L,l);

    // the order is same with macros : TYPE_* (defined top)
    lua_pushliteral(L, "data");
    lua_pushliteral(L, "more");
    lua_pushliteral(L, "error");
    lua_pushliteral(L, "open");
    lua_pushliteral(L, "close");
    lua_pushliteral(L, "warning");
    lua_pushcclosure(L, lfilter, 6);
    lua_setfield(L, -2, "filter");
    return 1;
}	
