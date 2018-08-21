/**************************************Copyright(c)****************************************
* ����������������            �ൺɽ���ǻ���Ϣ�Ƽ����޹�˾ ���������� ����                *
* ��������������������������                          ������������������������������������*
* ������������������                  www.iotsk.com ������������������                  ��*
* ������������������������������������������������������                          ��������*
*----------------------------------------File Info----------------------------------------*
*�� �� �� ��Ultrasound.c������������                                                      *
*�޸����� ��2013.11.21  ����������������������������                                      *
*�� �� �� ��V1.0����������������������������������                                        *
*��    �� ��������ģ���ն˽ڵ����                                                        *
*                                                                                         *
******************************************************************************************/

/******************************************************************************************
*                                       INCLUDES                                          *
******************************************************************************************/

#include "AF.h"
#include "OnBoard.h"
#include "OSAL_Tasks.h"
#include "ZDApp.h"
#include "ZDObject.h"
#include "ZDProfile.h"

#include "hal_drivers.h"
#include "hal_key.h"
#if defined ( LCD_SUPPORTED )
  #include "hal_lcd.h"
#endif
#include "hal_led.h"
#include "hal_uart.h"
#include "Public.h"
#include "Ultrasound.h"

/*******************************************************************************************
 *                                       MACROS
 ******************************************************************************************/

/*******************************************************************************************
 *                                     CONSTANTS
 ******************************************************************************************/

#if !defined( SERIAL_APP_PORT )
#define SERIAL_APP_PORT  0
#endif

#if !defined( SERIAL_APP_BAUD )
#define SERIAL_APP_BAUD  HAL_UART_BR_115200
#endif

// When the Rx buf space is less than this threshold, invoke the Rx callback.
#if !defined( SERIAL_APP_THRESH )
#define SERIAL_APP_THRESH  64
#endif

#if !defined( SERIAL_APP_RX_SZ )
#define SERIAL_APP_RX_SZ  128
#endif

#if !defined( SERIAL_APP_TX_SZ )
#define SERIAL_APP_TX_SZ  128
#endif

// Millisecs of idle time after a byte is received before invoking Rx callback.
#if !defined( SERIAL_APP_IDLE )
#define SERIAL_APP_IDLE  6
#endif

// Loopback Rx bytes to Tx for throughput testing.
#if !defined( SERIAL_APP_LOOPBACK )
#define SERIAL_APP_LOOPBACK  FALSE
#endif

// This is the max byte count per OTA message.
#if !defined( SERIAL_APP_TX_MAX )
#define SERIAL_APP_TX_MAX  20
#endif

#define SERIAL_APP_RSP_CNT  4

// This list should be filled with Application specific Cluster IDs.
const cId_t SerialApp_ClusterList[SERIALAPP_MAX_CLUSTERS] =
{
  SERIALAPP_CLUSTERID1,
  SERIALAPP_CLUSTERID2
};

const SimpleDescriptionFormat_t SerialApp_SimpleDesc =
{
  SERIALAPP_ENDPOINT,              //  int   Endpoint;
  SERIALAPP_PROFID,                //  uint16 AppProfId[2];
  SERIALAPP_DEVICEID,              //  uint16 AppDeviceId[2];
  SERIALAPP_DEVICE_VERSION,        //  int   AppDevVer:4;
  SERIALAPP_FLAGS,                 //  int   AppFlags:4;
  SERIALAPP_MAX_CLUSTERS,          //  byte  AppNumInClusters;
  (cId_t *)SerialApp_ClusterList,  //  byte *pAppInClusterList;
  SERIALAPP_MAX_CLUSTERS,          //  byte  AppNumOutClusters;
  (cId_t *)SerialApp_ClusterList   //  byte *pAppOutClusterList;
};

const endPointDesc_t SerialApp_epDesc =
{
  SERIALAPP_ENDPOINT,
 &SerialApp_TaskID,
  (SimpleDescriptionFormat_t *)&SerialApp_SimpleDesc,
  noLatencyReqs
};

/******************************************************************************************
 *                                     TYPEDEFS
 *****************************************************************************************/

/******************************************************************************************
 *                                GLOBAL VARIABLES
 *****************************************************************************************/

uint8 SerialApp_TaskID;    // Task ID for internal task/event processing.
devStates_t  SampleApp_NwkState;
static UART_Format UART0_Format;
static UART_Format_End4 UART0_Format1;

/*******************************************************************************************
 *                               EXTERNAL VARIABLES
 ******************************************************************************************/

/*******************************************************************************************
 *                               EXTERNAL FUNCTIONS
 *******************************************************************************************/

/********************************************************************************************
 *                                LOCAL VARIABLES
 *******************************************************************************************/

static uint8 SerialApp_MsgID;
static afAddrType_t SerialApp_TxAddr;
static uint8 SerialApp_TxBuf[SERIAL_APP_TX_MAX];
static uint8 SerialApp_TxLen;
/*******************************************************************************************
 *                                LOCAL FUNCTIONS
 *******************************************************************************************/

static void SerialApp_ProcessMSGCmd( afIncomingMSGPacket_t *pkt );
void SerialApp_OTAData(afAddrType_t *txaddr,uint8 ID,void *p,uint8 len);
static void SerialApp_CallBack(uint8 port, uint8 event);
static uint8 CheckSum(uint8 *data,uint8 len);
static void GPIO_Init(void);
extern void LED(uint8 led,uint8 operation);
extern void ledInit(void);
extern void FLASHLED(uint8 led);
__interrupt void P1_ISR(void);




/*****************************************************************************************
*�������� ��IO�ڳ�ʼ������				                                 
*��ڲ��� ����                                
*�� �� ֵ ����                                                                                
*˵    �� ����ʼ��IO�ڣ����򿪶�ʱ��1                                                             
*****************************************************************************************/

static void GPIO_Init(void)
{
  //P12����  P13���
  P1DIR &= ~0x04;
  P1DIR |= 0x08;
  TRIG   = 0;
  IEN2  |= 0x10;
  P1IEN |= 0x04;   //P12���ж�ʹ��
  PICTL &= ~0x02;  //P12������
  P1IFG &= 0x00;   //P1���ж�״̬,������жϷ�������Ӧλ��1
  T1CTL |= 0x08;   //T1ʹ��32��Ƶ
}

/*****************************************************************************************
*�������� ����ʱ��1�жϺ���				                                 
*��ڲ��� ����                                
*�� �� ֵ ����                                                                                
*˵    �� ������ʱֵ�ж�                                                             
*****************************************************************************************/

#pragma vector = P1INT_VECTOR
__interrupt void P1_ISR(void)  //P1���жϴ�������
{
  uint16 tempH = 0;
  uint8  tempL = 0;
  uint32 timeOutCnt = 0;
  uint16 echoTime;
  float  distanceMM;
  if(P1IFG & 0x04)  //p12
  {  
    T1CTL |= 0x01;    //������ʱ��
    P1IFG  = 0x00;    //���IO���жϱ�־
    while(ECHO)      //�ȴ��ز����ű�Ϊ0
    {
      timeOutCnt++;
      if(timeOutCnt > 480000) //�����ʱ�����أ�����ᵼ������
        return;
    }
    T1CTL  &= ~0x01;  //ֹͣ������
    tempH = T1CNTH;   //��ȡ������ֵ
    tempL = T1CNTL;
    echoTime = (tempH<<8)|tempL;
    T1CNTL = 0x00;    //����������
    if(echoTime > 100) //2cm�ڲ�����
    {
      distanceMM = echoTime*0.172; //��λΪmm,
      
      UART0_Format1.Command = 0x01;
      UART0_Format1.Data[0] = (uint16)distanceMM >> 8;
      UART0_Format1.Data[1] = (uint16)distanceMM;
      osal_set_event(SerialApp_TaskID, SERIALAPP_SEND_EVT);
    }
  }
  P1IF = 0;    
}

/*****************************************************************************************
*�������� �����ڳ�ʼ������					                          
*��ڲ��� ��task_id - OSAL���������ID��		                          
*�� �� ֵ ����							                          
*˵    �� ����OSAL��ʼ����ʱ�򱻵���                             
*****************************************************************************************/

void SerialApp_Init( uint8 task_id )
{
  halUARTCfg_t uartConfig;

  SerialApp_TaskID = task_id;

  afRegister( (endPointDesc_t *)&SerialApp_epDesc );

  RegisterForKeys( task_id );
  GPIO_Init();
  ledInit();
  
  uartConfig.configured           = TRUE;              // 2x30 don't care - see uart driver.
  uartConfig.baudRate             = SERIAL_APP_BAUD;
  uartConfig.flowControl          = FALSE;
  uartConfig.flowControlThreshold = SERIAL_APP_THRESH; // 2x30 don't care - see uart driver.
  uartConfig.rx.maxBufSize        = SERIAL_APP_RX_SZ;  // 2x30 don't care - see uart driver.
  uartConfig.tx.maxBufSize        = SERIAL_APP_TX_SZ;  // 2x30 don't care - see uart driver.
  uartConfig.idleTimeout          = SERIAL_APP_IDLE;   // 2x30 don't care - see uart driver.
  uartConfig.intEnable            = TRUE;              // 2x30 don't care - see uart driver.
  uartConfig.callBackFunc         = SerialApp_CallBack;
  HalUARTOpen (SERIAL_APP_PORT, &uartConfig);
  
  UART0_Format.Header   = '@';
  UART0_Format.Len      = 0x0a;
  UART0_Format.NodeSeq  = 0x01;
  UART0_Format.NodeID   = Ultrasound;
  
  UART0_Format1.Header   = '@';
  UART0_Format1.Len      = 0x0a;
  UART0_Format1.NodeSeq  = 0x01;
  UART0_Format1.NodeID   = Ultrasound;
  
  
  
  SerialApp_TxAddr.addrMode =(afAddrMode_t)Addr16Bit;//���͵�ַ��ʼ��
  SerialApp_TxAddr.endPoint = SERIALAPP_ENDPOINT;
  SerialApp_TxAddr.addr.shortAddr = 0x0000;
  TXPOWER = 0xf5;
}

/*****************************************************************************************
*�������� ���û������¼���������				                          
*��ڲ��� ��task_id - OSAL������¼�ID��                                      
*           events  - �¼�                                      
*�� �� ֵ ���¼���־		                          
*˵    �� ��                                                                              
*****************************************************************************************/

UINT16 SerialApp_ProcessEvent( uint8 task_id, UINT16 events )
{ 
  uint8 num=0;
  (void)task_id;  // Intentionally unreferenced parameter
  
  if ( events & SYS_EVENT_MSG )
  {
    afIncomingMSGPacket_t *MSGpkt;

    while ( (MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive( SerialApp_TaskID )) )
    {
      switch ( MSGpkt->hdr.event )
      {         
      case KEY_CHANGE:
        //SerialApp_HandleKeys( ((keyChange_t *)MSGpkt)->state, ((keyChange_t *)MSGpkt)->keys );
        break;

      case AF_INCOMING_MSG_CMD:
        SerialApp_ProcessMSGCmd( MSGpkt );
        FLASHLED(3);
        break;

      case ZDO_STATE_CHANGE:
          SampleApp_NwkState = (devStates_t)(MSGpkt->hdr.status);
          if(SampleApp_NwkState == DEV_END_DEVICE) //�ж���ǰ�豸����
          {
            
            osal_set_event(SerialApp_TaskID, PERIOD_EVT); //����������Ϣ
            osal_set_event(SerialApp_TaskID, ULTRASOUND_READ_EVT); //�������
            LED(1,ON);
          }
        break;
      default:
        break;
      }

      osal_msg_deallocate( (uint8 *)MSGpkt );
    }

    return ( events ^ SYS_EVENT_MSG );
  }
  
  if ( events & PERIOD_EVT ) //������Ϣ����
  {
    UART0_Format.Command = MSG_PERIOD;
    UART0_Format.Data[0] = 0x00;
    UART0_Format.Data[1] = 0x00; 
    UART0_Format.Data[2] = 0x00;
    UART0_Format.Data[3] = 0x00; 
    num = CheckSum(&UART0_Format.Header,UART0_Format.Len);
    UART0_Format.Verify  = num;

    SerialApp_OTAData(&SerialApp_TxAddr, SERIALAPP_CLUSTERID1, &UART0_Format, sizeof(UART_Format));
    osal_start_timerEx(SerialApp_TaskID, PERIOD_EVT, 5000);
    FLASHLED(2);
    return ( events ^ PERIOD_EVT );
  }
  
  if ( events & SERIALAPP_SEND_EVT ) //����RF��Ϣ
  { 
    num = CheckSum(&UART0_Format1.Header,UART0_Format1.Len);
    UART0_Format1.Verify  = num;

    SerialApp_OTAData(&SerialApp_TxAddr,SERIALAPP_CLUSTERID1, &UART0_Format1, sizeof(UART0_Format1));
    FLASHLED(4);
    return ( events ^ SERIALAPP_SEND_EVT );
  }

  if ( events & ULTRASOUND_READ_EVT )  
  {  
    TRIG = LOW;
    MicroWait(10);
    TRIG = HIGH;
    osal_start_timerEx(SerialApp_TaskID, ULTRASOUND_READ_EVT, 500);   
    return ( events ^ ULTRASOUND_READ_EVT );
  }

  return ( 0 );  // Discard unknown events.
}

/*****************************************************************************************
*�������� ����Ϣ��������				                                 
*��ڲ��� ��pkt   - ָ����յ���������Ϣ���ݰ���ָ��                                 
*�� �� ֵ ��TRUE  - ���ָ�뱻Ӧ�ò��ͷ�            
*           FALSE - ����	                                                          
*˵    �� �������յ���������Ϣ��������Ϣͨ�����ڷ��͸�����                               
*****************************************************************************************/

void SerialApp_ProcessMSGCmd( afIncomingMSGPacket_t *pkt )  //�������յ���RF��Ϣ
{ uint8 num=0;
  static UART_Format *receiveData;
  switch ( pkt->clusterId )
  {
   case SERIALAPP_CLUSTERID1:  //��������������������    
     receiveData = (UART_Format *)(pkt->cmd.Data);
     HalLedBlink(HAL_LED_1,1,50,200);
     
     num = CheckSum((uint8*)receiveData,receiveData->Len);
     
     if((receiveData->Header==0x40)&&(receiveData->Verify==num)) //У���ͷ��β

     {
       
     }
    break;

  case SERIALAPP_CLUSTERID2:
    break;

    default:
      break;
  }
}

/*****************************************************************************************
*�������� ��������Ϣ���ͺ���				                                 
*��ڲ��� ��*txaddr - ���͵�ַ
*           cID     - clusterID
*           *p      - �������ݰ��ĵ�ַ
*           len     - �������ݰ��ĳ���
*�� �� ֵ ����                                                                                
*˵    �� ������õ����ݰ���ͨ�����߷��ͳ�ȥ                                                            
*****************************************************************************************/


void SerialApp_OTAData(afAddrType_t *txaddr, uint8 cID, void *p, uint8 len) //���ͺ���
{
  if (afStatus_SUCCESS != AF_DataRequest(txaddr, //���͵�ַ
                                           (endPointDesc_t *)&SerialApp_epDesc, //endpoint����
                                            cID, //clusterID
                                            len, p, //�������ݰ��ĳ��Ⱥ͵�ַ
                                            &SerialApp_MsgID, 0, AF_DEFAULT_RADIUS))
  {
  }
  else
  {
    HalLedBlink(HAL_LED_1,1,50,200);
  }
}

/*****************************************************************************************
*�������� �����ڻص�����				                                 
*��ڲ��� ��port  -�˿ں�
*           event -�¼���
*�� �� ֵ ����                                                                              
*˵    �� ���Ѵ�����Ϣͨ�����߷��ͳ�ȥ                                                             
*****************************************************************************************/

static void SerialApp_CallBack(uint8 port, uint8 event)
{
  (void)port;

  if ((event & (HAL_UART_RX_FULL | HAL_UART_RX_ABOUT_FULL | HAL_UART_RX_TIMEOUT)) && !SerialApp_TxLen) //���ڽ��յ����ݰ�
  {
    SerialApp_TxLen = HalUARTRead(SERIAL_APP_PORT, SerialApp_TxBuf, SERIAL_APP_TX_MAX); //���������ݶ���buf
    SerialApp_TxLen = 0;  
  }
}

/*****************************************************************************************
*�������� ���ۼ�У��ͺ���				                                 
*��ڲ��� ��*data - ���ݰ���ַ
            len   - ���ݰ�����
*�� �� ֵ ��sum   - �ۼӺ�                                                                                
*˵    �� �������ݰ������ۼӺ�У��                                                             
*****************************************************************************************/

uint8 CheckSum(uint8 *data,uint8 len)
{
  uint8 i,sum=0;
  for(i=0;i<(len-1);i++)
  {
    sum+=data[i];
  }
  return sum;
}
/***********************************************/