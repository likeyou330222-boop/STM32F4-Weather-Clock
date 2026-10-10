#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H


#ifdef __cplusplus
extern "C" {
#endif


#include "aht20.h"



typedef enum
{
    SENSOR_TASK_OK = 0, //初始化成功
    SENSOR_TASK_ERROR_PARAM,//参数错误
    SENSOR_TASK_ERROR_QUEUE_CREATE,//FreeRTOS Queue 创建失败
    SENSOR_TASK_ERROR_TASK_CREATE,  //SensorTask 创建失败
    SENSOR_TASK_NO_DATA  //当前还没有有效的传感器数据
} SensorTask_Status_t;  //SensorTask 初始化状态


/*
 * ============================================================================
 * SensorTask_Init
 * ============================================================================
 * 创建传感器任务。
 * 成功后：
 *      SensorTask 会独立运行；周期性触发 AHT20 测量；读取温湿度；前阶段先通过串口打印结果。
 */
SensorTask_Status_t SensorTask_Init(void); //创建传感器任务


/*
 * ============================================================================
 * SensorTask_GetLatestMeasurement
 * ============================================================================
 * 获取 SensorTask 最近一次成功采集的温湿度数据。
 * measurement：   用于接收最新测量结果的结构体地址。
 * 返回：   SENSOR_TASK_OK  已经成功取得最新数据。
 *      SENSOR_TASK_NO_DATA    SensorTask 暂时还没有成功测量过。
 *      SENSOR_TASK_ERROR_PARAM     measurement == NULL。
 */
SensorTask_Status_t SensorTask_GetLatestMeasurement(AHT20_Measurement_t *measurement);






#ifdef __cplusplus
}
#endif


#endif


