#include "rtc.h"

#include "stm32f4xx.h"

#include <stddef.h>



/*
 * ============================================================================
 * RTC Driver 内部配置
 * ============================================================================
 */
#define RTC_LSE_STARTUP_TIMEOUT    1000000UL   //等待 LSE 起振时使用的最大循环次数

#define RTC_BACKUP_MAGIC_REGISTER    RTC_BKP_DR0  //使用备份域的RTC_BKP_DR0寄存器保存Magic Value

#define RTC_BACKUP_MAGIC_VALUE       0xA5A55A5AUL  //设定MAGIC_VALUE的值 若读取到 则认为时间成功设置



/*
 * ============================================================================
 * RTC 初始化
 * ============================================================================
 */
RTC_Status_t RTC_Driver_Init(void)
{
    uint32_t timeout;

    RTC_InitTypeDef rtc_init;

    /*
     * ------------------------------------------------------------------------
     * 第 1 步：开启 PWR 外设时钟
     * ------------------------------------------------------------------------
     * RTC 属于 STM32 的 Backup Domain。   Backup Domain（备份域：主电源没了以后，也尽量还能继续保存或运行的东西）
     * 想修改 Backup Domain，首先需要通过 PWR 外设获得访问权限。
     */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);

    /*
     * ------------------------------------------------------------------------
     * 第 2 步：允许访问 Backup Domain
     * ------------------------------------------------------------------------
     * STM32 默认会保护 RTC / Backup Domain，防止程序误操作。所以需要显式打开写访问。
     */
    PWR_BackupAccessCmd(ENABLE);

    /*
     * ------------------------------------------------------------------------
     * 第 3 步：启动 LSE
     * ------------------------------------------------------------------------
     * LSE: Low Speed External 低速外部时钟。
     * 我们板子上的 LSE 是：32.768 kHz
     */
    RCC_LSEConfig(RCC_LSE_ON);

     /*
     * ------------------------------------------------------------------------
     * 第 4 步：等待 LSE 真正稳定
     * ------------------------------------------------------------------------
     */
    timeout = RTC_LSE_STARTUP_TIMEOUT;
    while (RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET)  //检查LSERDY（LSE Ready）的状态位
    {
        /*
         * 如果已经等太久，
         * 说明 LSE 没有正常起振。
         */
        if (timeout == 0UL)
        {
            return RTC_STATUS_LSE_TIMEOUT;
        }


        --timeout;
    }

    /*
     * ------------------------------------------------------------------------
     * 第 5 步：选择 LSE 作为 RTC 时钟源
     * ------------------------------------------------------------------------
     */
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);

     /*
     * ------------------------------------------------------------------------
     * 第 6 步：正式打开 RTC Clock
     * ------------------------------------------------------------------------
     */
    RCC_RTCCLKCmd(ENABLE);

    /*
     * ------------------------------------------------------------------------
     * 第 7 步：等待 RTC 寄存器同步
     * ------------------------------------------------------------------------
     * RTC 在自己的低速时钟域运行， CPU / APB 在另外的高速时钟域运行。 所以需要等待二者同步。
     */
    if (RTC_WaitForSynchro() == ERROR)
    {
        return RTC_STATUS_SYNC_ERROR;
    }

    /*
     * ------------------------------------------------------------------------
     * 第 8 步：准备 RTC 配置结构体
     * ------------------------------------------------------------------------
     */
    RTC_StructInit(&rtc_init);  //此处使用了RTC_StructInit()的默认配置 即24H制 Async Prescaler = 127  Sync  Prescaler = 255

     /*
     * ------------------------------------------------------------------------
     * 第 9 步：初始化 RTC Calendar
     * ------------------------------------------------------------------------
     */
    if (RTC_Init(&rtc_init) == ERROR)
    {
        return RTC_STATUS_INIT_ERROR;
    }


    return RTC_STATUS_OK;
}


/*
 * ============================================================================
 * RTC 检查函数
 * ============================================================================
 */
uint8_t RTC_Driver_HasValidDateTime(void)
{
    /*
     * 读取 Backup Register 0。 如果里面还是我们之前写入的 Magic Value，就说明 RTC 曾经被成功设置过。
     */
    if (RTC_ReadBackupRegister(RTC_BACKUP_MAGIC_REGISTER) == RTC_BACKUP_MAGIC_VALUE)
    {
        return 1U;
    }

    return 0U;
}


/*
 * ============================================================================
 * 设置日期时间
 * ============================================================================
 */
RTC_Status_t RTC_Driver_SetDateTime(const RTC_DateTime_t *date_time)
{
    RTC_DateTypeDef date;  //日期
    RTC_TimeTypeDef time;  //时间

    /*
     * 防止空指针。
     */
    if (date_time == NULL)
    {
        return RTC_STATUS_INVALID_ARGUMENT;
    }

    /*
     * STM32F407 RTC 的年份字段只保存： 00 ~ 99
     * 我们的软件约定它代表：2000 ~ 2099
     */
    if ((date_time->year < 2000U) ||(date_time->year > 2099U))
    {
        return RTC_STATUS_INVALID_ARGUMENT;
    }


    /*
     * 基本范围检查。 
     * 更严格的：2月天数 闰年 大小月  我们后面再统一加强。
     */
    if ((date_time->month < 1U) ||(date_time->month > 12U) ||
        (date_time->day < 1U) ||(date_time->day > 31U) ||
        (date_time->hour > 23U) ||
        (date_time->minute > 59U) ||
        (date_time->second > 59U) ||
        (date_time->weekday < 1U) ||(date_time->weekday > 7U))
    {
        return RTC_STATUS_INVALID_ARGUMENT;
    }

    /*
     * 先初始化 STM32 官方日期结构体。
     */
    RTC_DateStructInit(&date);

    /*
     * 初始化时间结构体。
     */
    RTC_TimeStructInit(&time);

    /*
     * 把我们的 RTC_DateTime_t 转换成 STM32 RTC Driver 需要的格式。
     */
    date.RTC_Year    = (uint8_t)(date_time->year - 2000U);
    date.RTC_Month   = date_time->month;
    date.RTC_Date    = date_time->day;
    date.RTC_WeekDay = date_time->weekday;
    time.RTC_Hours   = date_time->hour;
    time.RTC_Minutes = date_time->minute;
    time.RTC_Seconds = date_time->second;

    /*
     * 写入日期。
     * RTC_Format_BIN 表示：我们传进去的是普通二进制数值，
     * 例如：22 就是真正十进制的 22。
     */
    if (RTC_SetDate(RTC_Format_BIN, &date) == ERROR)
    {
        return RTC_STATUS_SET_ERROR;
    }

    /*
     * 写入时间。
     */
    if (RTC_SetTime(RTC_Format_BIN, &time) == ERROR)
    {
        return RTC_STATUS_SET_ERROR;
    }

    /*
    * ------------------------------------------------------------
    * 日期和时间都已经成功写入。
    * 现在才在 Backup Register 中留下标记。
    * ------------------------------------------------------------
    */
    RTC_WriteBackupRegister(RTC_BACKUP_MAGIC_REGISTER, RTC_BACKUP_MAGIC_VALUE);

    return RTC_STATUS_OK;
}


/*
 * ============================================================================
 * 读取日期时间
 * ============================================================================
 */
RTC_Status_t RTC_Driver_GetDateTime(RTC_DateTime_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    /*
     * 检查调用者有没有真的提供存储空间。
     */
    if (date_time == NULL)
    {
        return RTC_STATUS_INVALID_ARGUMENT;
    }

    /*
     * ------------------------------------------------------------
     * 很重要：先读取 Time，再读取 Date。
     * ------------------------------------------------------------
     * STM32 RTC 使用 Shadow Register （影子寄存器）机制。
     * 按正确顺序读取 Time + Date，可以让这一组 Calendar 数据保持一致。
     */
    RTC_GetTime(RTC_Format_BIN, &time);
    RTC_GetDate(RTC_Format_BIN, &date);

    /*
     * 转换成我们自己的统一结构体。
     */
    date_time->year    =  2000U + date.RTC_Year;
    date_time->month   =  date.RTC_Month;
    date_time->day     =  date.RTC_Date;
    date_time->weekday =  date.RTC_WeekDay;
    date_time->hour    =  time.RTC_Hours;
    date_time->minute  =  time.RTC_Minutes;
    date_time->second  =  time.RTC_Seconds;

    return RTC_STATUS_OK;
}




