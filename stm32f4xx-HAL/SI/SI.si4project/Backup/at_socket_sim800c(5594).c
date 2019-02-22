/*
 * File      : at_socket_sim800c.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006 - 2018, RT-Thread Development Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-06-12     malongwei     first version
 */
#include <stdio.h>
#include <string.h>

#include <rtthread.h>
#include <rtdevice.h>
#include <sys/socket.h>

#include <at.h>
#include <at_socket.h>

#if !defined(AT_SW_VERSION_NUM) || AT_SW_VERSION_NUM < 0x10200
#error "This AT Client version is older, please check and update latest AT Client!"
#endif

#define LOG_TAG     "at.sim800c"
#define LOG_LVL     LOG_LVL_DBG
#include <at_log.h>

#ifdef AT_DEVICE_SIM800C


#define SIM800C_MODULE_SEND_MAX_SIZE       1000
#define SIM800C_WAIT_CONNECT_TIME          5000
#define SIM800C_THREAD_STACK_SIZE          1024
#define SIM800C_THREAD_PRIORITY            (RT_THREAD_PRIORITY_MAX/2)

/* set real event by current socket and current state */
#define SET_EVENT(socket, event)       (((socket + 1) << 16) | (event))

/* AT socket event type */
#define SIM800C_EVENT_CONN_OK              (1L << 0)
#define SIM800C_EVENT_SEND_OK              (1L << 1)
#define SIM800C_EVENT_RECV_OK              (1L << 2)
#define SIM800C_EVNET_CLOSE_OK             (1L << 3)
#define SIM800C_EVENT_CONN_FAIL            (1L << 4)
#define SIM800C_EVENT_SEND_FAIL            (1L << 5)

/* AT+CSTT command default*/
char *CSTT_CHINA_MOBILE  = "AT+CSTT=\"CMNET\"";
char *CSTT_CHINA_UNICOM  = "AT+CSTT=\"UNINET\"";
char *CSTT_CHINA_TELECOM = "AT+CSTT=\"CTNET\"";


uint8_t dial = 0;

static int cur_socket;
static rt_event_t at_socket_event;
static rt_mutex_t at_event_lock;
static at_evt_cb_t at_evt_cb_set[] = {
        [AT_SOCKET_EVT_RECV] = NULL,
        [AT_SOCKET_EVT_CLOSED] = NULL,
};

static int at_socket_event_send(uint32_t event)
{
    return (int) rt_event_send(at_socket_event, event);
}

static int at_socket_event_recv(uint32_t event, uint32_t timeout, rt_uint8_t option)
{
    int result = 0;
    rt_uint32_t recved;

    result = rt_event_recv(at_socket_event, event, option | RT_EVENT_FLAG_CLEAR, timeout, &recved);
    if (result != RT_EOK)
    {
        return -RT_ETIMEOUT;
    }

    return recved;
}

/**
 * close socket by AT commands.
 *
 * @param current socket
 *
 * @return  0: close socket success
 *         -1: send AT commands error
 *         -2: wait socket event timeout
 *         -5: no memory
 */
static int sim800c_socket_close(int socket)
{
    int result = 0;

    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);
    cur_socket = socket;

    if (at_exec_cmd(RT_NULL, "AT+CIPCLOSE=%d", socket) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    if (at_socket_event_recv(SET_EVENT(socket, SIM800C_EVNET_CLOSE_OK), rt_tick_from_millisecond(300*3), RT_EVENT_FLAG_AND) < 0)
    {
        LOG_E("socket (%d) close failed, wait close OK timeout.", socket);
        result = -RT_ETIMEOUT;
        goto __exit;
    }

__exit:
    rt_mutex_release(at_event_lock);

    return result;
}


/**
 * create TCP/UDP client or server connect by AT commands.
 *
 * @param socket current socket
 * @param ip server or client IP address
 * @param port server or client port
 * @param type connect socket type(tcp, udp)
 * @param is_client connection is client
 *
 * @return   0: connect success
 *          -1: connect failed, send commands error or type error
 *          -2: wait socket event timeout
 *          -5: no memory
 */
static int sim800c_socket_connect(int socket, char *ip, int32_t port, enum at_socket_type type, rt_bool_t is_client)
{
    int result = 0, event_result = 0;
    rt_bool_t retryed = RT_FALSE;

    RT_ASSERT(ip);
    RT_ASSERT(port >= 0);

    /* lock AT socket connect */
    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);

__retry:

    if (is_client)
    {
        switch (type)
        {
        case AT_SOCKET_TCP:
            /* send AT commands(eg: AT+QIOPEN=0,"TCP","x.x.x.x", 1234) to connect TCP server */
            if (at_exec_cmd(RT_NULL, "AT+CIPSTART=%d,\"TCP\",\"%s\",%d", socket, ip, port) < 0)
            {
                result = -RT_ERROR;
                goto __exit;
            }
            break;

        case AT_SOCKET_UDP:
            if (at_exec_cmd(RT_NULL, "AT+CIPSTART=%d,\"UDP\",\"%s\",%d", socket, ip, port) < 0)
            {
                result = -RT_ERROR;
                goto __exit;
            }
            break;

        default:
            LOG_E("Not supported connect type : %d.", type);
            result = -RT_ERROR;
            goto __exit;
            break;
        }
    }

    /* waiting result event from AT URC, the device default connection timeout is 75 seconds, but it set to 10 seconds is convenient to use.*/
    if (at_socket_event_recv(SET_EVENT(socket, 0), rt_tick_from_millisecond(10 * 1000), RT_EVENT_FLAG_OR) < 0)
    {
        LOG_E("socket (%d) connect failed, wait connect result timeout.", socket);
        result = -RT_ETIMEOUT;
        goto __exit;
    }
    /* waiting OK or failed result */
    if ((event_result = at_socket_event_recv(SIM800C_EVENT_CONN_OK | SIM800C_EVENT_CONN_FAIL, rt_tick_from_millisecond(1 * 1000),
            RT_EVENT_FLAG_OR)) < 0)
    {
        LOG_E("socket (%d) connect failed, wait connect OK|FAIL timeout.", socket);
        result = -RT_ETIMEOUT;
        goto __exit;
    }
    /* check result */
    if (event_result & SIM800C_EVENT_CONN_FAIL)
    {
        if (!retryed)
        {
            LOG_E("socket (%d) connect failed, maybe the socket was not be closed at the last time and now will retry.", socket);
            if (sim800c_socket_close(socket) < 0)
            {
                goto __exit;
            }
            retryed = RT_TRUE;
            goto __retry;
        }
        LOG_E("socket (%d) connect failed, failed to establish a connection.", socket);
        result = -RT_ERROR;
        goto __exit;
    }

__exit:
    /* unlock AT socket connect */
    rt_mutex_release(at_event_lock);

    return result;
}

/**
 * send data to server or client by AT commands.
 *
 * @param socket current socket
 * @param buff send buffer
 * @param bfsz send buffer size
 * @param type connect socket type(tcp, udp)
 *
 * @return >=0: the size of send success
 *          -1: send AT commands error or send data error
 *          -2: waited socket event timeout
 *          -5: no memory
 */
static int sim800c_socket_send(int socket, const char *buff, size_t bfsz, enum at_socket_type type)
{
    int result = 0, event_result = 0;
    at_response_t resp = RT_NULL;
    size_t cur_pkt_size = 0, sent_size = 0;

    RT_ASSERT(buff);

    resp = at_create_resp(128, 2, rt_tick_from_millisecond(5000));
    if (!resp)
    {
        LOG_E("No memory for response structure!");
        return -RT_ENOMEM;
    }

    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);

    /* set current socket for send URC event */
    cur_socket = socket;
    /* set AT client end sign to deal with '>' sign.*/
    at_set_end_sign('>');

    while (sent_size < bfsz)
    {
        if (bfsz - sent_size < SIM800C_MODULE_SEND_MAX_SIZE)
        {
            cur_pkt_size = bfsz - sent_size;
        }
        else
        {
            cur_pkt_size = SIM800C_MODULE_SEND_MAX_SIZE;
        }

        /* send the "AT+QISEND" commands to AT server than receive the '>' response on the first line. */
        if (at_exec_cmd(resp, "AT+CIPSEND=%d,%d", socket, cur_pkt_size) < 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* send the real data to server or client */
        result = (int) at_client_send(buff + sent_size, cur_pkt_size);
        if (result == 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* waiting result event from AT URC */
        if (at_socket_event_recv(SET_EVENT(socket, 0), rt_tick_from_millisecond(5 * 1000), RT_EVENT_FLAG_OR) < 0)
        {
            LOG_E("socket (%d) send failed, wait connect result timeout.", socket);
            result = -RT_ETIMEOUT;
            goto __exit;
        }
        /* waiting OK or failed result */
        if ((event_result = at_socket_event_recv(SIM800C_EVENT_SEND_OK | SIM800C_EVENT_SEND_FAIL, rt_tick_from_millisecond(5 * 1000),
                RT_EVENT_FLAG_OR)) < 0)
        {
            LOG_E("socket (%d) send failed, wait connect OK|FAIL timeout.", socket);
            result = -RT_ETIMEOUT;
            goto __exit;
        }
        /* check result */
        if (event_result & SIM800C_EVENT_SEND_FAIL)
        {
            LOG_E("socket (%d) send failed, return failed.", socket);
            result = -RT_ERROR;
            goto __exit;
        }

        if (type == AT_SOCKET_TCP)
        {
            //cur_pkt_size = cur_send_bfsz;
        }

        sent_size += cur_pkt_size;
    }


__exit:
    /* reset the end sign for data conflict */
    at_set_end_sign(0);

    rt_mutex_release(at_event_lock);

    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

/**
 * domain resolve by AT commands.
 *
 * @param name domain name
 * @param ip parsed IP address, it's length must be 16
 *
 * @return  0: domain resolve success
 *         -1: send AT commands error or response error
 *         -2: wait socket event timeout
 *         -5: no memory
 */
static int sim800c_domain_resolve(const char *name, char ip[16])
{
#define RESOLVE_RETRY                  5

    int i, result = RT_EOK;
    char recv_ip[16] = { 0 };
    at_response_t resp = RT_NULL;

    RT_ASSERT(name);
    RT_ASSERT(ip);

    /* The maximum response time is 14 seconds, affected by network status */
    resp = at_create_resp(128, 4, rt_tick_from_millisecond(14 * 1000));
    if (!resp)
    {
        LOG_E("No memory for response structure!");
        return -RT_ENOMEM;
    }

    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);

    for(i = 0; i < RESOLVE_RETRY; i++)
    {
        if (at_exec_cmd(resp, "AT+CDNSGIP=\"%s\"", name) < 0)
        {
            result = -RT_ERROR;
            goto __exit;
        }

        /* parse the third line of response data, get the IP address */
        if(at_resp_parse_line_args_by_kw(resp, "+CDNSGIP:", "%*[^,],%*[^,],\"%[^\"]", recv_ip) < 0)
        {
            rt_thread_delay(rt_tick_from_millisecond(100));
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }

        if (strlen(recv_ip) < 8)
        {
            rt_thread_delay(rt_tick_from_millisecond(100));
            /* resolve failed, maybe receive an URC CRLF */
            continue;
        }
        else
        {
            strncpy(ip, recv_ip, 15);
            ip[15] = '\0';
            //LOG_I("DNS IP:%s",ip);
            break;
        }
    }

__exit:
    rt_mutex_release(at_event_lock);

    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;

}

/**
 * set AT socket event notice callback
 *
 * @param event notice event
 * @param cb notice callback
 */
static void sim800c_socket_set_event_cb(at_socket_evt_t event, at_evt_cb_t cb)
{
    if (event < sizeof(at_evt_cb_set) / sizeof(at_evt_cb_set[1]))
    {
        at_evt_cb_set[event] = cb;
    }
}

static void urc_connect_func(const char *data, rt_size_t size)
{
    int socket = 0;

    RT_ASSERT(data && size);

    sscanf(data, "%d,%*", &socket);

    if (strstr(data, "CONNECT OK"))
    {
        at_socket_event_send(SET_EVENT(socket, SIM800C_EVENT_CONN_OK));
    }
    else if (strstr(data, "CONNECT FAIL"))
    {
        at_socket_event_send(SET_EVENT(socket, SIM800C_EVENT_CONN_FAIL));
    }
}

static void urc_send_func(const char *data, rt_size_t size)
{
    RT_ASSERT(data && size);

    if (strstr(data, "SEND OK"))
    {
        at_socket_event_send(SET_EVENT(cur_socket, SIM800C_EVENT_SEND_OK));
    }
    else if (strstr(data, "SEND FAIL"))
    {
        at_socket_event_send(SET_EVENT(cur_socket, SIM800C_EVENT_SEND_FAIL));
    }
}

static void urc_close_func(const char *data, rt_size_t size)
{
    int socket = 0;

    RT_ASSERT(data && size);

    if (strstr(data, "CLOSE OK"))
    {
        at_socket_event_send(SET_EVENT(cur_socket, SIM800C_EVNET_CLOSE_OK));
    }
    else if (strstr(data, "CLOSED"))
    {
        sscanf(data, "%d, CLOSED", &socket);
        /* notice the socket is disconnect by remote */
        if (at_evt_cb_set[AT_SOCKET_EVT_CLOSED])
        {
            at_evt_cb_set[AT_SOCKET_EVT_CLOSED](socket, AT_SOCKET_EVT_CLOSED, NULL, 0);
        }
    }
}

static void urc_recv_func(const char *data, rt_size_t size)
{
    int socket = 0;
    rt_size_t bfsz = 0, temp_size = 0;
    rt_int32_t timeout;
    char *recv_buf = RT_NULL, temp[8];

    RT_ASSERT(data && size);

    /* get the current socket and receive buffer size by receive data */
    sscanf(data, "+RECEIVE,%d,%d:", &socket, (int *) &bfsz);
    /* get receive timeout by receive buffer length */
    timeout = bfsz;

    if (socket < 0 || bfsz == 0)
        return;

    recv_buf = rt_calloc(1, bfsz);
    if (!recv_buf)
    {
        LOG_E("no memory for URC receive buffer (%d)!", bfsz);
        /* read and clean the coming data */
        while (temp_size < bfsz)
        {
            if (bfsz - temp_size > sizeof(temp))
            {
                at_client_recv(temp, sizeof(temp), timeout);
            }
            else
            {
                at_client_recv(temp, bfsz - temp_size, timeout);
            }
            temp_size += sizeof(temp);
        }
        return;
    }

    /* sync receive data */
    if (at_client_recv(recv_buf, bfsz, timeout) != bfsz)
    {
        LOG_E("receive size(%d) data failed!", bfsz);
        rt_free(recv_buf);
        return;
    }

    /* notice the receive buffer and buffer size */
    if (at_evt_cb_set[AT_SOCKET_EVT_RECV])
    {
        at_evt_cb_set[AT_SOCKET_EVT_RECV](socket, AT_SOCKET_EVT_RECV, recv_buf, bfsz);
    }
}


static void urc_func(const char *data, rt_size_t size)
{
    RT_ASSERT(data);

    LOG_I("URC data : %.*s", size, data);
}

static const struct at_urc urc_table[] = {
        {"RDY",         "\r\n",                 urc_func},
        {"",            ", CONNECT OK\r\n",     urc_connect_func},
        {"",            ", CONNECT FAIL\r\n",   urc_connect_func},
        {"",            ", SEND OK\r\n",        urc_send_func},
        {"",            ", SEND FAIL\r\n",      urc_send_func},
        {"",            ", CLOSE OK\r\n",       urc_close_func},
        {"",            ", CLOSED\r\n",         urc_close_func},
        {"+RECEIVE,",   "\r\n",                 urc_recv_func},
};

#define AT_SEND_CMD(resp, resp_line, timeout, cmd)                                                              \
    do                                                                                                          \
    {                                                                                                           \
        if (at_exec_cmd(at_resp_set_info(resp, 128, resp_line, rt_tick_from_millisecond(timeout)), cmd) < 0)    \
        {                                                                                                       \
            result = -RT_ERROR;                                                                                 \
            goto __exit;                                                                                        \
        }                                                                                                       \
    } while(0);                                                                                                 \

static void sim800c_power_on(void)
{
    if (rt_pin_read(AT_DEVICE_STATUS_PIN) == PIN_HIGH)
        return;
    rt_pin_write(AT_DEVICE_POWER_PIN, PIN_HIGH);
    while (rt_pin_read(AT_DEVICE_STATUS_PIN) == PIN_LOW)
    {
        rt_thread_delay(rt_tick_from_millisecond(10));
    }
    rt_pin_write(AT_DEVICE_POWER_PIN, PIN_LOW);
}

static void sim800c_power_off(void)
{
    if (rt_pin_read(AT_DEVICE_STATUS_PIN) == PIN_LOW)
        return;
    rt_pin_write(AT_DEVICE_POWER_PIN, PIN_HIGH);
    while (rt_pin_read(AT_DEVICE_STATUS_PIN) == PIN_HIGH)
    {
        rt_thread_delay(rt_tick_from_millisecond(10));
    }
    rt_pin_write(AT_DEVICE_POWER_PIN, PIN_LOW);
}

/* init for SIM800C */
static void sim800c_init_thread_entry(void *parameter)
{
#define CPIN_RETRY                     10
#define CSQ_RETRY                      10
#define CREG_RETRY                     10
#define CGREG_RETRY                    20

    at_response_t resp = RT_NULL;
    int i, qimux;
    char parsed_data[10];
    rt_err_t result = RT_EOK;

    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);

    while (1)
    {
        result = RT_EOK;
        rt_memset(parsed_data, 0, sizeof(parsed_data));
//        rt_thread_delay(rt_tick_from_millisecond(500));
//        sim800c_power_on();
//        rt_thread_delay(rt_tick_from_millisecond(2000));
        resp = at_create_resp(128, 0, rt_tick_from_millisecond(300));
        if (!resp)
        {
            LOG_E("No memory for response structure!");
            result = -RT_ENOMEM;
            goto __exit;
        }
        LOG_D("Start initializing the SIM800C module");
        /* wait SIM800C startup finish */
        if (at_client_wait_connect(SIM800C_WAIT_CONNECT_TIME))
        {
            result = -RT_ETIMEOUT;
            goto __exit;
        }
        /* disable echo */
        AT_SEND_CMD(resp, 0, 300, "ATE0");
        /* get module version */
        AT_SEND_CMD(resp, 0, 300, "ATI");
        /* show module version */
        for (i = 0; i < (int)resp->line_counts - 1; i++)
        {
            LOG_D("%s", at_resp_get_line(resp, i + 1));
        }
        /* check SIM card */
        for (i = 0; i < CPIN_RETRY; i++)
        {
            at_exec_cmd(at_resp_set_info(resp, 128, 2, rt_tick_from_millisecond(5000)), "AT+CPIN?");

            if (at_resp_get_line_by_kw(resp, "READY"))
            {
                LOG_D("SIM card detection success");
                break;
            }
            rt_thread_delay(rt_tick_from_millisecond(1000));
        }
        if (i == CPIN_RETRY)
        {
            LOG_E("SIM card detection failed!");
            result = -RT_ERROR;
            goto __exit;
        }
        /* waiting for dirty data to be digested */
        rt_thread_delay(rt_tick_from_millisecond(10));

        /* check the GSM network is registered */
        for (i = 0; i < CREG_RETRY; i++)
        {
            AT_SEND_CMD(resp, 0, 300, "AT+CREG?");
            at_resp_parse_line_args_by_kw(resp, "+CREG:", "+CREG: %s", &parsed_data);
            if (!strncmp(parsed_data, "0,1", sizeof(parsed_data)) || !strncmp(parsed_data, "0,5", sizeof(parsed_data)))
            {
                LOG_D("GSM network is registered (%s)", parsed_data);
                break;
            }
            rt_thread_delay(rt_tick_from_millisecond(1000));
        }
        if (i == CREG_RETRY)
        {
            LOG_E("The GSM network is register failed (%s)", parsed_data);
            result = -RT_ERROR;
            goto __exit;
        }
        /* check the GPRS network is registered */
        for (i = 0; i < CGREG_RETRY; i++)
        {
            AT_SEND_CMD(resp, 0, 300, "AT+CGREG?");
            at_resp_parse_line_args_by_kw(resp, "+CGREG:", "+CGREG: %s", &parsed_data);
            if (!strncmp(parsed_data, "0,1", sizeof(parsed_data)) || !strncmp(parsed_data, "0,5", sizeof(parsed_data)))
            {
                LOG_D("GPRS network is registered (%s)", parsed_data);
                break;
            }
            rt_thread_delay(rt_tick_from_millisecond(1000));
        }
        if (i == CGREG_RETRY)
        {
            LOG_E("The GPRS network is register failed (%s)", parsed_data);
            result = -RT_ERROR;
            goto __exit;
        }

        /* check signal strength */
        for (i = 0; i < CSQ_RETRY; i++)
        {
            AT_SEND_CMD(resp, 0, 300, "AT+CSQ");
            at_resp_parse_line_args_by_kw(resp, "+CSQ:", "+CSQ: %s", &parsed_data);
            if (strncmp(parsed_data, "99,99", sizeof(parsed_data)))
            {
                LOG_D("Signal strength: %s", parsed_data);
                break;
            }
            rt_thread_delay(rt_tick_from_millisecond(1000));
        }
        if (i == CSQ_RETRY)
        {
            LOG_E("Signal strength check failed (%s)", parsed_data);
            result = -RT_ERROR;
            goto __exit;
        }

		AT_SEND_CMD(resp, 0, 5000, "AT+CGCLASS=\"B\"");
//		rt_thread_delay(rt_tick_from_millisecond(3000));
		AT_SEND_CMD(resp, 0, 5000, "AT+CGDCONT=1,\"IP\",\"UNIM2M.NJM2MAPN\"");
//		AT_SEND_CMD(resp, 0, 5000, "AT+CGDCONT=1,\"IP\",\"CMNET\"");
        AT_SEND_CMD(resp, 0, 1000, "AT+CGATT=1");

//		rt_thread_delay(rt_tick_from_millisecond(3000));

		
		AT_SEND_CMD(resp, 2, 10 * 5000, "ATDT*99#");
//		AT_SEND_CMD(resp, 2, 10 * 5000, "ATDT*99***1#");
		if (at_resp_get_line_by_kw(resp, "CONNECT") == RT_NULL)
		{
			LOG_E("Dial sim800c failed");
            result = -RT_ERROR;
            goto __exit;
		}

		dial = 1;
		
#if 0
        /* the device default response timeout is 40 seconds, but it set to 15 seconds is convenient to use. */
        AT_SEND_CMD(resp, 2, 20 * 1000, "AT+CIPSHUT");

        /* Set to multiple connections */
        AT_SEND_CMD(resp, 0, 300, "AT+CIPMUX?");
        at_resp_parse_line_args_by_kw(resp, "+CIPMUX:", "+CIPMUX: %d", &qimux);
        if (qimux == 0)
        {
            AT_SEND_CMD(resp, 0, 300, "AT+CIPMUX=1");
        }

        AT_SEND_CMD(resp, 0, 300, "AT+COPS?");
        at_resp_parse_line_args_by_kw(resp, "+COPS:", "+COPS: %*[^\"]\"%[^\"]", &parsed_data);
        if (strcmp(parsed_data, "CHINA MOBILE") == 0)
        {
            /* "CMCC" */
            LOG_I("%s", parsed_data);
            AT_SEND_CMD(resp, 0, 300, CSTT_CHINA_MOBILE);
        }
        else if (strcmp(parsed_data, "CHN-UNICOM") == 0)
        {
            /* "UNICOM" */
            LOG_I("%s", parsed_data);
            AT_SEND_CMD(resp, 0, 300, CSTT_CHINA_UNICOM);
        }
        else if (strcmp(parsed_data, "CHN-CT") == 0)
        {
            AT_SEND_CMD(resp, 0, 300, CSTT_CHINA_TELECOM);
            /* "CT" */
            LOG_I("%s", parsed_data);
        }

        /* the device default response timeout is 150 seconds, but it set to 20 seconds is convenient to use. */
        AT_SEND_CMD(resp, 0, 20 * 1000, "AT+CIICR");

        AT_SEND_CMD(resp, 2, 300, "AT+CIFSR");
        if(at_resp_get_line_by_kw(resp, "ERROR") != RT_NULL)
        {
            LOG_E("Get the local address failed");
            result = -RT_ERROR;
            goto __exit;
        }
#endif
    __exit:
        if (resp)
        {
            at_delete_resp(resp);
        }

        if (!result)
        {
            LOG_I("AT network initialize success!");
            rt_mutex_release(at_event_lock);
            break;
        }
        else
        {
            LOG_E("AT network initialize failed (%d)!", result);
//            sim800c_power_off();
        }
    }
}

int sim800c_net_init(void)
{
#ifdef PKG_AT_INIT_BY_THREAD
    rt_thread_t tid;

    tid = rt_thread_create("sim800c_net_init", sim800c_init_thread_entry, RT_NULL, SIM800C_THREAD_STACK_SIZE, SIM800C_THREAD_PRIORITY, 20);
    if (tid)
    {
        rt_thread_startup(tid);
    }
    else
    {
        LOG_E("Create AT initialization thread fail!");
    }
#else
    sim800c_init_thread_entry(RT_NULL);
#endif

    return RT_EOK;
}

int sim800c_ping(int argc, char **argv)
{
    LOG_E("This device does not support Ping!");
    return RT_EOK;
}

int sim800c_ifconfig(void)
{
    at_response_t resp = RT_NULL;
    char resp_arg[AT_CMD_MAX_LEN] = { 0 };
    const char * resp_expr = "%s";
    rt_err_t result = RT_EOK;

    resp = at_create_resp(64, 2, rt_tick_from_millisecond(300));
    if (!resp)
    {
        rt_kprintf("No memory for response structure!\n");
        return -RT_ENOMEM;
    }

    rt_mutex_take(at_event_lock, RT_WAITING_FOREVER);
    if (at_exec_cmd(resp, "AT+CIFSR") < 0)
    {
        rt_kprintf("AT send ip commands error!\n");
        result = RT_ERROR;
        goto __exit;
    }

    if (at_resp_parse_line_args(resp, 2, resp_expr, resp_arg) == 1)
    {
        rt_kprintf("IP address : %s\n", resp_arg);
    }
    else
    {
        rt_kprintf("Parse error, current line buff : %s\n", at_resp_get_line(resp, 2));
        result = RT_ERROR;
        goto __exit;
    }

__exit:
    rt_mutex_release(at_event_lock);
    
    if (resp)
    {
        at_delete_resp(resp);
    }

    return result;
}

#ifdef FINSH_USING_MSH
#include <finsh.h>
MSH_CMD_EXPORT_ALIAS(sim800c_net_init, at_net_init, initialize AT network);
MSH_CMD_EXPORT_ALIAS(sim800c_ping, at_ping, AT ping network host);
MSH_CMD_EXPORT_ALIAS(sim800c_ifconfig, at_ifconfig, list the information of network interfaces);
#endif

static const struct at_device_ops sim800c_socket_ops = {
    sim800c_socket_connect,
    sim800c_socket_close,
    sim800c_socket_send,
    sim800c_domain_resolve,
    sim800c_socket_set_event_cb,
};

static int at_socket_device_init(void)
{
    /* create current AT socket event */
    at_socket_event = rt_event_create("at_se", RT_IPC_FLAG_FIFO);
    if (at_socket_event == RT_NULL)
    {
        LOG_E("AT client port initialize failed! at_sock_event create failed!");
        return -RT_ENOMEM;
    }

    /* create current AT socket event lock */
    at_event_lock = rt_mutex_create("at_se", RT_IPC_FLAG_FIFO);
    if (at_event_lock == RT_NULL)
    {
        LOG_E("AT client port initialize failed! at_sock_lock create failed!");
        rt_event_delete(at_socket_event);
        return -RT_ENOMEM;
    }

    /* initialize AT client */
//    at_client_init(AT_DEVICE_NAME, AT_DEVICE_RECV_BUFF_LEN);
	at_client_init("uart3", AT_DEVICE_RECV_BUFF_LEN);

    /* register URC data execution function  */
    at_set_urc_table(urc_table, sizeof(urc_table) / sizeof(urc_table[0]));

    /* initialize sim800c network */
//    rt_pin_mode(AT_DEVICE_POWER_PIN, PIN_MODE_OUTPUT);
//    rt_pin_mode(AT_DEVICE_STATUS_PIN, PIN_MODE_INPUT);
    sim800c_net_init();

    /* set sim800c AT Socket options */
//    at_socket_device_register(&sim800c_socket_ops);

    return RT_EOK;
}
INIT_APP_EXPORT(at_socket_device_init);

#endif /* AT_DEVICE_SIM800C */
