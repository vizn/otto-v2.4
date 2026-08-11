#ifndef OTTO_CONTROLLER_H
#define OTTO_CONTROLLER_H

struct HardwareConfig;

// 花花舞/打节拍/随机舞蹈 音乐联动入口（由 miot_client / music_player 调用）
void OttoControllerQueueFlowerDance();
void OttoControllerQueueBeatKeeping();
void OttoControllerQueueRandomDance();
bool OttoControllerAvailable();

#endif // OTTO_CONTROLLER_H
