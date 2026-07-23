#ifndef PARKING_ALERT_UAPI_H
#define PARKING_ALERT_UAPI_H

/*
 * /dev/parking_alert 공용 ABI.
 * 이 헤더는 Linux 커널 모듈과 C/C++ 사용자 프로그램이 함께 사용한다.
 */
#include <linux/ioctl.h>
#include <linux/types.h>

#define PARKING_ALERT_API_VERSION 1U
#define PARKING_ALERT_MAX_SLOTS 32U
#define PARKING_ALERT_NO_SLOT ((__u32)~0U)

enum parking_alert_operation {
    PARKING_ALERT_OP_SET_SLOT = 1,
    PARKING_ALERT_OP_CLEAR_SLOT = 2,
    PARKING_ALERT_OP_CLEAR_ALL = 3,
};

/* write() 및 SET/CLEAR ioctl에 전달하는 고정 크기 명령이다. */
struct parking_alert_command {
    __u16 version;
    __u16 operation;
    __u32 slot_index;
    __u64 event_id;
};

/* read() 및 GET_STATE ioctl로 조회하는 현재 장치 상태다. */
struct parking_alert_state {
    __u16 version;
    __u16 reserved;
    __u32 active_mask;
    __u32 last_operation;
    __u32 last_slot;
    __u64 generation;
    __u64 last_event_id;
};

#define PARKING_ALERT_IOC_MAGIC 'P'
#define PARKING_ALERT_IOC_SET_SLOT \
    _IOW(PARKING_ALERT_IOC_MAGIC, 0x01, struct parking_alert_command)
#define PARKING_ALERT_IOC_CLEAR_SLOT \
    _IOW(PARKING_ALERT_IOC_MAGIC, 0x02, struct parking_alert_command)
#define PARKING_ALERT_IOC_CLEAR_ALL _IO(PARKING_ALERT_IOC_MAGIC, 0x03)
#define PARKING_ALERT_IOC_GET_STATE \
    _IOR(PARKING_ALERT_IOC_MAGIC, 0x04, struct parking_alert_state)

#endif
