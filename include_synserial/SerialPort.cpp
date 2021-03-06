#include "SerialPort.h"
#include "process.h"

#include <string>
#include <iostream>

using namespace std;

char g_uart_rec_data[10240];
const int   WAITTIME_PER_BYTE = 100;   //读每个字节时间(ms)

CSerialPort::CSerialPort() :m_hListenThread(INVALID_HANDLE_VALUE)
{
	m_hComm = INVALID_HANDLE_VALUE;
	m_hListenThread = INVALID_HANDLE_VALUE;

	/*
	临界区在使用时以CRITICAL_SECTION结构对象保护共享资源，并分别用EnterCriticalSection（）和LeaveCriticalSection（）函数去标识和释放一个临界区。
	所用到的CRITICAL_SECTION结构对象必须经过InitializeCriticalSection（）的初始化后才能使用，
	而且必须确保所有线程中的任何试图访问此共享资源的代码都处在此临界区的保护之下。
	否则临界区将不会起到应有的作用，共享资源依然有被破坏的可能
	*/
	//InitializeCriticalSection(&m_csCommunicationSync);

}

CSerialPort::~CSerialPort(void)
{
	CloseListenThread();
	closePort();
	//DeleteCriticalSection(&m_csCommunicationSync);
}

bool CSerialPort::openPort(UINT portNo)
{
	/** 进入临界段 */
	//EnterCriticalSection(&m_csCommunicationSync);

	/** 把串口的编号转换为设备名 */
	char szPort[50];
	sprintf_s(szPort, "\\\\.\\COM%d", portNo);

	m_hComm = CreateFileA(szPort,     /** 设备名,COM1,COM2等 */
		GENERIC_READ | GENERIC_WRITE, /** 访问模式,可同时读写 */
		0,                            /** 共享模式,0表示不共享 */
		NULL,                         /** 安全性设置,一般使用NULL */
		OPEN_EXISTING,                /** 该参数表示设备必须存在,否则创建失败 */
		0,
		0);

	if (m_hComm == INVALID_HANDLE_VALUE)
	{
		//LeaveCriticalSection(&m_csCommunicationSync);
		return false;
	}

	//LeaveCriticalSection(&m_csCommunicationSync);
	return true;


}

void CSerialPort::closePort()
{
	/** 如果有串口被打开，关闭它 */
	if (m_hComm != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_hComm);
		s_bExit = false;
		//LeaveCriticalSection(&m_csCommunicationSync);
		m_hComm = INVALID_HANDLE_VALUE;
	}

}

bool CSerialPort::isOpen()
{
	return m_hComm != INVALID_HANDLE_VALUE;
}

UartDataRecvIn* m_callback;
bool CSerialPort::InitPort(UINT portNO, UINT baud, char parity, UINT databit, UINT stopsbits, DWORD dwCommEvents)
{
	/** 临时变量,将制定参数转化为字符串形式,以构造DCB结构 */
	char szDCBparam[50];
	sprintf_s(szDCBparam, "baud=%d parity=%c data=%d, stop=%d", baud, parity, databit, stopsbits);

	/** 打开指定串口,该函数内部已经有临界区保护,上面请不要加保护 */
	if (!openPort(portNO))
	{
		return false;
	}

	/** 进入临界段 */
	//EnterCriticalSection(&m_csCommunicationSync);

	/** 是否有错误发生 */
	bool bIsSuccess = true;

	COMMTIMEOUTS CommTimeouts;
	CommTimeouts.ReadIntervalTimeout = 100;
	CommTimeouts.ReadTotalTimeoutMultiplier = 100;  // WAITTIME_PER_BYTE;
	CommTimeouts.ReadTotalTimeoutConstant = 100;
	CommTimeouts.WriteTotalTimeoutMultiplier = 0;
	CommTimeouts.WriteTotalTimeoutConstant = 100;

	if (bIsSuccess)
	{
		bIsSuccess = SetCommTimeouts(m_hComm, &CommTimeouts);
	}

	DCB m_dcb;
	if (bIsSuccess)
	{
		// 将ANSI字符串转换为UNICODE字符串
		DWORD dwNum = MultiByteToWideChar(CP_ACP, 0, szDCBparam, -1, NULL, 0);
		wchar_t *pwText = new wchar_t[dwNum];
		if (!MultiByteToWideChar(CP_ACP, 0, szDCBparam, -1, pwText, dwNum))
		{
			bIsSuccess = true;
		}


		/** 获取当前串口配置参数,并且构造串口DCB参数 */
		bIsSuccess = GetCommState(m_hComm, &m_dcb) ;


		///** 开启RTS flow控制 */
		//m_dcb.fRtsControl = RTS_CONTROL_ENABLE;

		delete[] pwText;

	}

	if (bIsSuccess)
	{
		m_dcb.DCBlength = sizeof(DCB);
		m_dcb.BaudRate = baud;
		m_dcb.Parity = NOPARITY;
		m_dcb.ByteSize = 8;
		m_dcb.StopBits = 0;

		/** 使用DCB参数配置串口状态 */
		bIsSuccess = SetCommState(m_hComm,&m_dcb);
	}

	/**  清空串口缓冲区 */
	PurgeComm(m_hComm, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

	//LeaveCriticalSection(&m_csCommunicationSync);
	s_bExit = true;
	
	return bIsSuccess == true;

}

bool CSerialPort::InitPort(UINT portNo, const LPDCB& plDCB)
{


	return false;
}


bool CSerialPort::OpenListenThread(UartDataRecvIn* callback)
{
	if (m_hListenThread != INVALID_HANDLE_VALUE)
	{
		return false;
	}

	s_bExit = false;
	m_callback = callback;
	thread_exit = false;
	UINT threadid;
	m_hListenThread = (HANDLE)_beginthreadex(NULL, 0, ListenThread, this, 0, &threadid);
	if (!m_hListenThread)
	{
		return false;
	}

	if (!SetThreadPriority(m_hListenThread, THREAD_PRIORITY_ABOVE_NORMAL))
	{
		return false;
	}

	return true;
}


UINT WINAPI CSerialPort::ListenThread(void *pParam)
{
	/** 得到本类的指针 */
	CSerialPort *pSerialPort = reinterpret_cast<CSerialPort*>(pParam);


	// 线程循环,轮询方式读取串口数据
	while (!pSerialPort->thread_exit)
	{

		UINT ByteInQue = pSerialPort->GetBytesInCOM();
		/** 如果串口输入缓冲区中无数据,则休息一会再查询 */
		if (ByteInQue == 0)
		{
			Sleep(5);
			continue;
		}

		/** 读取输入缓冲区中的数据并输出显示 */
		char Ctemp = 0x00;
		int i = 0;
		memset(g_uart_rec_data, 0, 10240);
		do
		{
			Ctemp = 0x00;
			if (pSerialPort->ReadChar(Ctemp) == true)
			{
				g_uart_rec_data[i] = Ctemp;
				i++;
				
				continue;
			}
		} while (--ByteInQue);

		m_callback->OnRecvData(g_uart_rec_data);
	}
	return 0;
}

bool CSerialPort::CloseListenThread()
{
	if (m_hListenThread != INVALID_HANDLE_VALUE)
	{
		/** 通知线程退出 */
		thread_exit = true;

		/** 等待线程退出 */
		Sleep(10);

		/** 置线程句柄无效 */
		//CloseHandle(m_hListenThread);
		m_hListenThread = INVALID_HANDLE_VALUE;
	}

	return true;
}

UINT CSerialPort::GetBytesInCOM()
{
	DWORD dwError = 0;
	COMSTAT comstat;   /** COMSTAT结构体,记录通信设备的状态信息 */
	memset(&comstat, 0, sizeof(COMSTAT));

	UINT ByteInQue = 0;

	/** 在调用ReadFile和WriteFile之前,通过本函数清除以前遗留的错误标志 */
	if (ClearCommError(m_hComm, &dwError, &comstat))
	{
		ByteInQue = comstat.cbInQue; /** 获取在输入缓冲区中的字节数 */
	}

	return ByteInQue;
}

int CSerialPort::ReadData(char *cRecved,int len)
{
	bool bResult = true;
	DWORD  BytesRead = 0;
	if (m_hComm == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	/** 临界区保护 */
	//EnterCriticalSection(&m_csCommunicationSync);

	/** 从缓冲区读取一个字节的数据 */
	bResult = ReadFile(m_hComm, cRecved, len, &BytesRead, NULL);

	if (!bResult)
	{
		/** 获取错误码,可以根据该错误码查出错误原因 */
		DWORD dwError = GetLastError();

		/** 清空串口缓冲区 */
		PurgeComm(m_hComm, PURGE_RXCLEAR | PURGE_RXABORT);

		//LeaveCriticalSection(&m_csCommunicationSync);
		return 0;
	}

	/** 离开临界区 */
	//LeaveCriticalSection(&m_csCommunicationSync);
	return BytesRead;
}

bool CSerialPort::ReadChar(char &cRecved)
{
	bool bResult = true;
	DWORD  BytesRead = 0;
	if (m_hComm == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	/** 临界区保护 */
	//EnterCriticalSection(&m_csCommunicationSync);

	/** 从缓冲区读取一个字节的数据 */
	bResult = ReadFile(m_hComm, &cRecved, 1, &BytesRead, NULL);
	if (!bResult)
	{
		/** 获取错误码,可以根据该错误码查出错误原因 */
		DWORD dwError = GetLastError();
		/** 清空串口缓冲区 */
		PurgeComm(m_hComm, PURGE_RXCLEAR | PURGE_RXABORT);

		//LeaveCriticalSection(&m_csCommunicationSync);
		return false;
	}

	/** 离开临界区 */
	//LeaveCriticalSection(&m_csCommunicationSync);
	return BytesRead == 1;
}



bool CSerialPort::WriteData(char*pData, unsigned int length)
{
	bool bResult = true;
	DWORD BytesToSend = 0;
	if (m_hComm == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	///** 临界区保护 */
	//EnterCriticalSection(&m_csCommunicationSync);

	/** 向缓冲区写入指定量的数据 */
	bResult = WriteFile(m_hComm, pData, length, &BytesToSend, NULL);

	if (!bResult)
	{
		/** 获取错误码,可以根据该错误码查出错误原因 */
		DWORD dwError = GetLastError();

		/** 清空串口缓冲区 */
		PurgeComm(m_hComm, PURGE_RXCLEAR | PURGE_RXABORT);

		//LeaveCriticalSection(&m_csCommunicationSync);
		return false;
	}

	/** 离开临界区 */
	//LeaveCriticalSection(&m_csCommunicationSync);
	return length == BytesToSend;

}
