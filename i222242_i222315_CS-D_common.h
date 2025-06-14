// common.h

#ifndef COMMON_H
#define COMMON_H

#define MQ_PORTAL_STATUS "/mq_portal_status"
#define MQ_PORTAL_TO_SMART "/mq_portal_to_smart"
#define MQ_SMART_TO_PORTAL "/mq_smart_to_portal"

struct PortalStatusMsg {
    char status[20];
};

struct PortalCommandMsg {
    char command[50];
};

struct ChallanMsg {
    char vehicleID[20];
    bool paid;
};

#endif // COMMON_H

