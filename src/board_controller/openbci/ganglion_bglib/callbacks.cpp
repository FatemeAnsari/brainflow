#include <ctype.h>
#include <math.h>
#include <deque>
#include <stdlib.h>
#include <string.h>

#include "cmd_def.h"
#include "ganglion_types.h"
#include "helpers.h"
#include "timestamp.h"
#include "uart.h"
#include "ticket_lock.h"

#define GANGLION_SERVICE_UUID 0xc566488a08824e1ba6d00b717e652234 
#define CLIENT_CHARACTERISTIC_UUID 0x2902

namespace GanglionLib
{
    extern volatile int exit_code;
    extern volatile bd_addr connect_addr;
    extern volatile uint8 connection;
    extern volatile uint16 ganglion_handle_start;
    extern volatile uint16 ganglion_handle_end;
    // recv and send chars handles
    extern volatile uint16 ganglion_handle_recv;
    extern volatile uint16 ganglion_handle_send;
    extern volatile uint16 client_char_handle;
    extern volatile State state;
    extern TicketLock lock;

    extern std::deque<struct GanglionLib::GanglionData> data_queue;

	// uuid="4051eb11-bf0a-4c74-8730-a48f4193fcea" - Commands BITalino
    const int send_char_uuid_bytes[16] = {
        234, 252, 147, 65, 143, 164, 48, 135, 116, 76, 10, 191, 17, 235, 81, 64};
    // uuid = "40fdba6b-672e-47c4-808a-e529adff3633" - Frames
    const int recv_char_uuid_bytes[16] = {
        51, 54, 255, 173, 41, 229, 138, 128, 196, 71, 46, 103, 107, 186, 253, 64};
}

void ble_evt_gap_scan_response (const struct ble_msg_gap_scan_response_evt_t *msg)
{
    char name[512];
    bool name_found_in_response = false;
    for (int i = 0; i < msg->data.len;)
    {
        int8 len = msg->data.data[i++];
        if (!len)
        {
            continue;
        }
        if (i + len > msg->data.len)
        {
            break; // not enough data
        }
        uint8 type = msg->data.data[i++];
        if (type == 0x09) // no idea what is 0x09
        {
            name_found_in_response = true;
            memcpy (name, msg->data.data + i, len - 1);
            name[len - 1] = '\0';
        }

        i += len - 1;
    }

    if (name_found_in_response)
    {
        // strcasestr is unavailable for windows
        if (strstr (name, "anglion") != NULL)
        {
            memcpy ((void *)GanglionLib::connect_addr.addr, msg->sender.addr, sizeof (bd_addr));
            GanglionLib::exit_code = (int)GanglionLib::STATUS_OK;
        }
    }
}

void ble_evt_connection_status (const struct ble_msg_connection_status_evt_t *msg)
{
    // New connection
    if (msg->flags & connection_connected)
    {
        GanglionLib::connection = msg->connection;
        // this method is called from ble_evt_connection_disconnected need to set exit code only
        // when we call this method from open_ble_device
        if (GanglionLib::state == GanglionLib::State::INITIAL_CONNECTION)
        {
            GanglionLib::exit_code = (int)GanglionLib::STATUS_OK;
        }
    }
}

void ble_evt_connection_disconnected (const struct ble_msg_connection_disconnected_evt_t *msg)
{
    // atempt to reconnect
    // changing values here leads to package loss, dont touch it
    ble_cmd_gap_connect_direct (
        &GanglionLib::connect_addr, gap_address_type_random, 10, 76, 100, 0);
}

// ble_evt_attclient_group_found and ble_evt_attclient_procedure_completed are called after the same
// command(ble_cmd_attclient_read_by_group_type)
void ble_evt_attclient_group_found (const struct ble_msg_attclient_group_found_evt_t *msg)
{
    if (msg->uuid.len == 0)
    {
        return;
    }
    uint16 uuid = (msg->uuid.data[1] << 8) | msg->uuid.data[0];
    if (msg->uuid.len == 16)
    {
        GanglionLib::ganglion_handle_start = msg->start;
        GanglionLib::ganglion_handle_end = msg->end;
    }
}

void ble_evt_attclient_procedure_completed (
    const struct ble_msg_attclient_procedure_completed_evt_t *msg)
{
    if (GanglionLib::state == GanglionLib::State::WRITE_TO_CLIENT_CHAR)
    {
        if (msg->result == 0)
        {
            GanglionLib::exit_code = (int)GanglionLib::STATUS_OK;
        }
    }
    if (GanglionLib::state == GanglionLib::State::OPEN_CALLED)
    {
        if ((GanglionLib::ganglion_handle_start) && (GanglionLib::ganglion_handle_end))
        {
            ble_cmd_attclient_find_information (msg->connection, GanglionLib::ganglion_handle_start,
                GanglionLib::ganglion_handle_end); // triggers
                                                   // ble_evt_attclient_find_information_found
        }
    }
    else if (GanglionLib::state == GanglionLib::State::CONFIG_CALLED)
    {
        if (msg->result == 0)
        {
            GanglionLib::exit_code = (int)GanglionLib::STATUS_OK;
        }
    }
}

// finds characteristic handles and set exit_code for open_ganglion call
void ble_evt_attclient_find_information_found (
    const struct ble_msg_attclient_find_information_found_evt_t *msg)
{
    if (GanglionLib::state == GanglionLib::State::OPEN_CALLED)
    {
        if ((GanglionLib::ganglion_handle_recv) && msg->uuid.len == 2)
        {
            uint16 uuid = (msg->uuid.data[1] << 8) | msg->uuid.data[0];
            if (uuid == CLIENT_CHARACTERISTIC_UUID)
            {
                GanglionLib::client_char_handle = msg->chrhandle;
            }
        }
        if (msg->uuid.len == 16)
        {
            bool is_send = true;
            bool is_recv = true;
            for (int i = 0; i < 16; i++)
            {
                if (msg->uuid.data[i] != GanglionLib::send_char_uuid_bytes[i])
                {
                    is_send = false;
                }
                if (msg->uuid.data[i] != GanglionLib::recv_char_uuid_bytes[i])
                {
                    is_recv = false;
                }
            }
            if (is_recv)
            {
                GanglionLib::ganglion_handle_recv = msg->chrhandle;
            }
            if (is_send)
            {
                GanglionLib::ganglion_handle_send = msg->chrhandle;
            }
        }
        if ((GanglionLib::ganglion_handle_send) && (GanglionLib::ganglion_handle_recv) &&
            (GanglionLib::client_char_handle) &&
            (GanglionLib::state == GanglionLib::State::OPEN_CALLED))
        {
            GanglionLib::exit_code = (int)GanglionLib::STATUS_OK;
        }
    }
}

void ble_evt_attclient_attribute_value (const struct ble_msg_attclient_attribute_value_evt_t *msg)
{
    unsigned char values[20] = {0};
    memcpy (values, msg->value.data, msg->value.len * sizeof (unsigned char));
    double timestamp = get_timestamp ();
    struct GanglionLib::GanglionData data (values, timestamp);
    GanglionLib::lock.lock ();
    GanglionLib::data_queue.push_back (data);
    GanglionLib::lock.unlock ();

}
