#include <stdbool.h>
#include <stdint.h>
#include "scheduler.h"
#include "wifi_hal.h"
#include "wifi_mgr.h"
#include "wifi_em.h"
#include "wifi_em_utils.h"
#include "const.h"

#define DCA_TO_APP 1
#define APP_TO_DCA 2

typedef struct {
    sta_data_t      assoc_stats[BSS_MAX_NUM_STATIONS];
    bool            threshold_hit[BSS_MAX_NUM_STATIONS];
    unsigned int    hit_count;
    size_t          stat_array_size;
} client_assoc_data_t;

typedef struct {
    client_assoc_data_t client_assoc_data[MAX_NUM_VAP_PER_RADIO];
    unsigned int    assoc_stats_vap_presence_mask;
    unsigned int    req_stats_vap_mask;
} client_assoc_stats_t;

client_assoc_stats_t client_assoc_stats[MAX_NUM_RADIOS];

static int em_rssi_to_rcpi (int rssi)
{
    if (!rssi)
        return 255;
    if (rssi < -110)
        return 0;
    if (rssi > 0)
        return 220;
    return (rssi + 110)*2;
}

static unsigned em_get_radio_index_from_mac(mac_addr_t ruuid)
{
    unsigned num_of_radios = getNumberRadios();
    wifi_vap_info_map_t *vap_map;

    for (int i = 0; i < num_of_radios; i++)
    {
        vap_map = (wifi_vap_info_map_t *)get_wifidb_vap_map(i);
        for (int j = 0; j < vap_map->num_vaps; j++)
        {
            if (memcmp(ruuid, vap_map->vap_array[j].u.bss_info.bssid, sizeof(mac_addr_t)))
                return vap_map->vap_array[j].radio_index;
        }
    }
}

static int em_match_radio_index_to_policy_index(radio_metrics_policies_t *radio_metrics_policies, unsigned radio_index)
{
    int radio_count = radio_metrics_policies->radio_count;
    unsigned found_index;
    for (int i = 0; i < radio_count; i++)
    {
        found_index = em_get_radio_index_from_mac(radio_metrics_policies->radio_metrics_policy[i].ruid);
        if (found_index == radio_index)
            return i;
    }
}

int em_common_config_to_monitor_queue(wifi_app_t *app, wifi_monitor_data_t *data)
{
    unsigned index;
    int radio_count = app->data.u.em_data.em_config.radio_metrics_policies.radio_count;
    for (int i = 0; i< radio_count; i++)
    {
        data[i].u.mon_stats_config.inst = wifi_app_inst_easymesh;

        index = em_get_radio_index_from_mac(app->data.u.em_data.em_config.radio_metrics_policies.radio_metrics_policy[i].ruid);

        data[i].u.mon_stats_config.args.radio_index = index;
        data[i].u.mon_stats_config.interval_ms = app->data.u.em_data.em_config.ap_metric_policy.interval*1000; //converting seconds to ms
    }
    return RETURN_OK;
}

int em_route(wifi_event_route_t *route)
{
    memset(route, 0, sizeof(wifi_event_route_t));
    route->dst = wifi_sub_component_mon;
    route->u.inst_bit_map = wifi_app_inst_easymesh;
    return RETURN_OK;
}

static int prepare_sta_lins_metrics_data(webconfig_subdoc_data_t *data, client_assoc_data_t *stats)
{
    int sta_count = 0;
    int sta_it = 0;
    for (int i = 0; i < MAX_NUM_VAP_PER_RADIO; i++)
    {
        sta_count += stats[i].hit_count;
        stats[i].hit_count = 0;
    }

    data->u.decoded.em_sta_link_metrics_rsp.sta_count = sta_count;
    data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics = (em_per_sta_metrics_t *)malloc(sta_count * sizeof(em_per_sta_metrics_t));
    if (data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics == NULL) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d Error in allocating table for encode stats\n", __func__,
            __LINE__);
        free(data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics);
        free(data);
        return RETURN_ERR;
    }

    em_per_sta_metrics_t * param = data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics;

    for (int i = 0; i < MAX_NUM_VAP_PER_RADIO; i++)
    {
        for (int j = 0; j < stats[i].stat_array_size; j++)
        {
            if (stats[i].threshold_hit[j] == true)
            {
                stats[i].threshold_hit[j] = false;
                
                // Associated STA Link Metrics
                memcpy(param[sta_it].assoc_sta_link_metrics.sta_mac, stats[i].assoc_stats[j].sta_mac, sizeof(mac_address_t));
                param[sta_it].assoc_sta_link_metrics.num_bssid = 1; //must be changed for STA multiple associations

                memcpy(param[sta_it].assoc_sta_link_metrics.assoc_sta_link_metrics_data[0].bssid, stats[i].assoc_stats[j].link_mac);
                param[sta_it].assoc_sta_link_metrics.assoc_sta_link_metrics_data[0].time_delta = 0; //How to calculate time Delta (The time delta in ms between the time at which the earliest measurement that contributed to the data rate estimates were made, and the time at which this report was sent.)
                param[sta_it].assoc_sta_link_metrics.assoc_sta_link_metrics_data[0].est_mac_rate_down = stats[i].assoc_stats[j].dev_stats.cli_MaxDownlinkRate; // I'm not sure if cli_MaxXXXX is the same as "Estimated MAC Data Rate in downlink"
                param[sta_it].assoc_sta_link_metrics.assoc_sta_link_metrics_data[0].est_mac_rate_up = stats[i].assoc_stats[j].dev_stats.cli_MaxUplinkRate;
                param[sta_it].assoc_sta_link_metrics.assoc_sta_link_metrics_data[0].rcpi = em_rssi_to_rcpi(stats[i].assoc_stats[j].dev_stats.cli_RSSI);


                // Associated STA Extended Link Metrics 
                memcpy(param[sta_it].assoc_sta_ext_link_metrics.sta_mac, stats[i].assoc_stats[j].sta_mac, sizeof(mac_address_t));
                param[sta_it].assoc_sta_ext_link_metrics.num_bssid = 1; //must be changed for STA multiple associations
                memcpy(param[sta_it].assoc_sta_ext_link_metrics.assoc_sta_ext_link_metrics_data[0].bssid, stats[i].assoc_stats[j].link_mac);
                param[sta_it].assoc_sta_ext_link_metrics.assoc_sta_ext_link_metrics_data[0].last_data_downlink_rate = stats[i].assoc_stats[j].dev_stats.cli_LastDataDownlinkRate;
                param[sta_it].assoc_sta_ext_link_metrics.assoc_sta_ext_link_metrics_data[0].last_data_uplink_rate = stats[i].assoc_stats[j].dev_stats.cli_LastDataUplinkRate;
                param[sta_it].assoc_sta_ext_link_metrics.assoc_sta_ext_link_metrics_data[0].utilization_receive = 0; //do we have that data?
                param[sta_it].assoc_sta_ext_link_metrics.assoc_sta_ext_link_metrics_data[0].utilization_transmit = 0; //do we have that data?
                sta_it++;
            }
        }
    }
    return RETURN_OK;
}

static int em_sta_stats_publish(wifi_app_t *app, client_assoc_data_t *stats, unsigned int vap_index)
{
    webconfig_subdoc_data_t *data;
    raw_data_t rdata;
    int rc;

    wifi_ctrl_t *ctrl = (wifi_ctrl_t *)get_wifictrl_obj();
    data = (webconfig_subdoc_data_t *)malloc(sizeof(webconfig_subdoc_data_t));
    if (data == NULL) {
        wifi_util_error_print(WIFI_APPS,
            "%s: malloc failed to allocate webconfig_subdoc_data_t, size %d\n", __func__,
            sizeof(webconfig_subdoc_data_t));
        return -1;
    }


    //need to specify how to pack all the metrics, send one by one or into array?
    memset(data, 0, sizeof(webconfig_subdoc_data_t));
    memset(&rdata, 0, sizeof(raw_data_t));
    data->u.decoded.em_sta_link_metrics_rsp.vap_index = vap_index;
    prepare_sta_lins_metrics_data(data, stats);

    if (webconfig_encode(&ctrl->webconfig, data, webconfig_subdoc_type_em_sta_link_metrics) != webconfig_error_none) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d Error in encoding assocdev stats\n", __func__,
            __LINE__);
        free(data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics);
        free(data);
        return RETURN_ERR;
    }

    rdata.data_type = bus_data_type_string;
    rdata.raw_data.bytes = (void *)data->u.encoded.raw;
    rdata.raw_data_len = strlen(data->u.encoded.raw) + 1;

    rc = get_bus_descriptor()->bus_event_publish_fn(&app->handle, "Device.WiFi.CollectStats.AccessPoint.1.AssociatedDeviceStats", &rdata);
    if (rc != bus_error_success) {
        wifi_util_error_print(WIFI_CTRL, "%s:%d: bus: bus_event_publish_fn Event failed %d\n",
            __func__, __LINE__, rc);
        free(data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics);
        free(data);
        return RETURN_ERR;
    }
    free(data->u.decoded.em_sta_link_metrics_rsp.per_sta_metrics);
    free(data);

}

static int handle_ready_client_stats(wifi_app_t *app, client_assoc_data_t *stats, size_t stats_num, unsigned int vap_mask, unsigned int radio_index, unsigned int vap_index)
{
    unsigned int tmp_vap_index = 0;
    int tmp_vap_array_index = 0;
    wifi_mgr_t *wifi_mgr = get_wifimgr_obj();
    int RCPI;
    int policy_index = em_match_radio_index_to_policy_index(&app->data.u.em_data.em_config.radio_metrics_policies, radio_index);
    int RCPI_threshold = app->data.u.em_data.em_config.radio_metrics_policies.radio_metrics_policy[policy_index].sta_rcpi_threshold;
    int RCPI_hysteresis = app->data.u.em_data.em_config.radio_metrics_policies.radio_metrics_policy[policy_index].sta_rcpi_hysteresis;
    wifi_app_t *wifi_app = NULL;
    

    if (!stats) {
        wifi_util_error_print(WIFI_EM,"%s:%d: stats is NULL for radio_index: %d\r\n",__func__, __LINE__, radio_index);
        return RETURN_ERR;
    }

    while (vap_mask) {
        /* check all VAPs */
        if (vap_mask & 0x1) {
            tmp_vap_array_index = convert_vap_index_to_vap_array_index(&wifi_mgr->hal_cap.wifi_prop, tmp_vap_index);
            if (tmp_vap_array_index >= 0 && tmp_vap_array_index < (int)stats_num) {
                size_t stat_array_size = stats[tmp_vap_array_index].stat_array_size;
                stats[tmp_vap_array_index].hit_count = 0;
                for (size_t i = 0; i < stat_array_size; i++) {
                    sta_data_t *sta_data = &stats[tmp_vap_array_index].assoc_stats[i];
                    if (!sta_data) {
                        continue;
                    }
                    if (sta_data->dev_stats.cli_Active == false) {
                        continue;
                    }
                    RCPI = em_rssi_to_rcpi(sta_data->dev_stats.cli_RSSI);
                    if (RCPI < RCPI_threshold)
                    {
                        stats[tmp_vap_array_index].threshold_hit[i] = true;
                        stats[tmp_vap_array_index].hit_count++;
                    }
                    else if (stats[tmp_vap_array_index].threshold_hit[i] == true && RCPI < (RCPI_threshold + RCPI_hysteresis))
                    {
                        stats[tmp_vap_array_index].threshold_hit[i] = true;
                        stats[tmp_vap_array_index].hit_count++;
                    }
                    else
                    {
                        stats[tmp_vap_array_index].threshold_hit[i] = false;
                    }
                }
            }
        }
        tmp_vap_index++;
        vap_mask >>= 1;
    }
    em_sta_stats_publish(app, stats, vap_index);
    return RETURN_OK;
}

int assoc_client_response(wifi_app_t *app, wifi_provider_response_t *provider_response)
{
    unsigned int radio_index = 0;
    unsigned int vap_index = 0;
    int vap_array_index = 0;
    radio_index = provider_response->args.radio_index;
    vap_index = provider_response->args.vap_index;
    wifi_mgr_t *wifi_mgr = get_wifimgr_obj();
    char vap_name[32];

    if (convert_vap_index_to_name(&wifi_mgr->hal_cap.wifi_prop, vap_index, vap_name) != RETURN_OK) {
        wifi_util_error_print(WIFI_EM,"%s:%d: convert_vap_index_to_name failed for vap_index : %d\r\n",__func__, __LINE__, vap_index);
        return RETURN_ERR;
    }

    vap_array_index = convert_vap_name_to_array_index(&wifi_mgr->hal_cap.wifi_prop, vap_name);
    if (vap_array_index == -1) {
        wifi_util_error_print(WIFI_EM,"%s:%d: convert_vap_name_to_array_index failed for vap_name: %s\r\n",__func__, __LINE__, vap_name);
        return RETURN_ERR;
    }

    memset(client_assoc_stats[radio_index].client_assoc_data[vap_array_index].assoc_stats, 0, sizeof(client_assoc_stats[radio_index].client_assoc_data[vap_array_index].assoc_stats));
    memcpy(client_assoc_stats[radio_index].client_assoc_data[vap_array_index].assoc_stats, provider_response->stat_pointer, (sizeof(sta_data_t)*provider_response->stat_array_size));
    client_assoc_stats[radio_index].client_assoc_data[vap_array_index].stat_array_size = provider_response->stat_array_size;
    client_assoc_stats[radio_index].assoc_stats_vap_presence_mask |= (1 << vap_index);

    wifi_util_dbg_print(WIFI_EM,"%s:%d: vap_index : %d client array size : %d \r\n",__func__, __LINE__, vap_index, provider_response->stat_array_size);

    if ((client_assoc_stats[radio_index].assoc_stats_vap_presence_mask == client_assoc_stats[radio_index].req_stats_vap_mask)) {
        wifi_util_dbg_print(WIFI_EM,"%s:%d: push to dpp for radio_index : %d \r\n",__func__, __LINE__, radio_index);
        handle_ready_client_stats(app, client_assoc_stats[radio_index].client_assoc_data,
                                  MAX_NUM_VAP_PER_RADIO,
                                  client_assoc_stats[radio_index].assoc_stats_vap_presence_mask,
                                  radio_index, vap_index);
        client_assoc_stats[radio_index].assoc_stats_vap_presence_mask = 0;
    }

    return RETURN_OK;
}

int handle_monitor_provider_response(wifi_app_t *app, wifi_event_t *event)
{
    wifi_provider_response_t    *provider_response;
    provider_response = (wifi_provider_response_t *)event->u.provider_response;
    int ret = RETURN_ERR;

    if (provider_response == NULL) {
        wifi_util_error_print(WIFI_EM,"%s:%d: input event is NULL\r\n",__func__, __LINE__);
        return ret;
    }

    switch (provider_response->args.app_info) {

        case em_app_event_type_assoc_dev_stats:
            ret = assoc_client_response(app, provider_response);
        break;
        default:
            wifi_util_error_print(WIFI_EM,"%s:%d: event not handle[%d]\r\n",__func__, __LINE__, provider_response->args.app_info);
    }

    return ret;
}

int monitor_event_em(wifi_app_t *app, wifi_event_t *event)
{
    int ret = RETURN_ERR;

    if (event == NULL) {
        wifi_util_error_print(WIFI_EM,"%s:%d: input event is NULL\r\n",__func__, __LINE__);
        return ret;
    }

    switch (event->sub_type) {
        case wifi_event_monitor_provider_response:
            ret = handle_monitor_provider_response(app, event);
        break;
        default:
            wifi_util_error_print(WIFI_EM,"%s:%d: event not handle[%d]\r\n",__func__, __LINE__, event->sub_type);
        break;
    }

    return ret;
}

int generate_vap_mask_for_radio_index(unsigned int radio_index)
{
   rdk_wifi_vap_map_t *rdk_vap_map = NULL;
   unsigned int count = 0;
   rdk_vap_map = getRdkWifiVap(radio_index);
   if (rdk_vap_map == NULL) {
       wifi_util_error_print(WIFI_EM,"%s:%d: getRdkWifiVap failed for radio_index : %d\r\n",__func__, __LINE__, radio_index);
       return RETURN_ERR;
   }
   for (count = 0; count < rdk_vap_map->num_vaps; count++) {
       if (!isVapSTAMesh(rdk_vap_map->rdk_vap_array[count].vap_index)) {
           client_assoc_stats[radio_index].req_stats_vap_mask |= (1 << rdk_vap_map->rdk_vap_array[count].vap_index);
       }
   }

    return RETURN_OK;
}

int client_diag_config_to_monitor_queue(wifi_app_t *app, wifi_monitor_data_t *data)
{
    unsigned int vapArrayIndex = 0;
    wifi_mgr_t *wifi_mgr = get_wifimgr_obj();
    wifi_event_route_t route;
    em_route(&route);
    if (em_common_config_to_monitor_queue(app, data) != RETURN_OK) {
        wifi_util_error_print(WIFI_EM,"%s:%d em Config creation failed %d\r\n", __func__, __LINE__, stats_type_client);
        return RETURN_ERR;
    }

    int radio_count = app->data.u.em_data.em_config.radio_metrics_policies.radio_count;

    for (int i = 0; i < radio_count; i++)
    {
        data[i].u.mon_stats_config.data_type = mon_stats_type_associated_device_stats;

        if (client_assoc_stats[data[i].u.mon_stats_config.args.radio_index].req_stats_vap_mask == 0) {
            if(generate_vap_mask_for_radio_index(data[i].u.mon_stats_config.args.radio_index) == RETURN_ERR) {
                wifi_util_error_print(WIFI_EM,"%s:%d generate_vap_mask_for_radio_index failed \r\n", __func__, __LINE__);
                return RETURN_ERR;
            }
        }

        data[i].u.mon_stats_config.args.app_info = em_app_event_type_assoc_dev_stats;

        //for each vap push the event to monitor queue
        for (vapArrayIndex = 0; vapArrayIndex < getNumberVAPsPerRadio(data[i].u.mon_stats_config.args.radio_index); vapArrayIndex++) {
            data[i].u.mon_stats_config.args.vap_index = wifi_mgr->radio_config[data[i].u.mon_stats_config.args.radio_index].vaps.rdk_vap_array[vapArrayIndex].vap_index;
            if (!isVapSTAMesh(data[i].u.mon_stats_config.args.vap_index)) {
                push_event_to_monitor_queue(data + i, wifi_event_monitor_data_collection_config, &route);
            }
        }
    }

    return RETURN_OK;
}

int push_em_config_event_to_monitor_queue(wifi_app_t *app, wifi_mon_stats_request_state_t state)
{
    wifi_monitor_data_t *data;
    int ret = RETURN_ERR;
    int radio_count = app->data.u.em_data.em_config.radio_metrics_policies.radio_count;

    data = (wifi_monitor_data_t *)malloc(radio_count * sizeof(wifi_monitor_data_t));
    if (data == NULL) {
        wifi_util_error_print(WIFI_EM,"%s:%d data allocation failed\r\n", __func__, __LINE__);
        return RETURN_ERR;
    }
    memset(data, 0, radio_count * sizeof(wifi_monitor_data_t));

    for (int i = 0; i < radio_count; i++)
    {
        data[i].u.mon_stats_config.req_state = state;
    }

    ret = client_diag_config_to_monitor_queue(app, data);

    if (ret == RETURN_ERR) {
        wifi_util_error_print(WIFI_EM,"%s:%d Event trigger failed for %d\r\n", __func__, __LINE__, stats_type_client);
        free(data);
        return RETURN_ERR;
    }

    free(data);

    return RETURN_OK;
}

int handle_em_webconfig_event(wifi_app_t *app, wifi_event_t *event)
{

    webconfig_subdoc_data_t *webconfig_data = NULL;
    if (event == NULL) {
        wifi_util_dbg_print(WIFI_EM,"%s %d input arguements are NULL\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    webconfig_data = event->u.webconfig_data;
    if (webconfig_data == NULL) {
        wifi_util_dbg_print(WIFI_EM,"%s %d webconfig_data is NULL\n", __func__, __LINE__);
        return RETURN_ERR;
    }

    if (webconfig_data->type != webconfig_subdoc_type_em_config) {
        return RETURN_ERR;
    }


    em_config_t *new_policy_cfg = &webconfig_data->u.decoded.em_config;
    em_data_t *current_policy_cfg = &app->data.u.em_data;
    int temp_count = 0;
    bool size_change;


    current_policy_cfg->em_config.ap_metric_policy = new_policy_cfg->ap_metric_policy;
    current_policy_cfg->em_config.backhaul_bss_config_policy = new_policy_cfg->backhaul_bss_config_policy;

    temp_count = new_policy_cfg->btm_steering_dslw_policy.sta_count;
    current_policy_cfg->em_config.btm_steering_dslw_policy.sta_count = temp_count;

    if (temp_count != 0)
    {
        memcpy(current_policy_cfg->em_config.btm_steering_dslw_policy.disallowed_sta, new_policy_cfg->btm_steering_dslw_policy.disallowed_sta, temp_count * sizeof(mac_addr_t));
    }

    current_policy_cfg->em_config.channel_scan_reporting_policy = new_policy_cfg->channel_scan_reporting_policy;

    temp_count = new_policy_cfg->local_steering_dslw_policy.sta_count;
    current_policy_cfg->em_config.local_steering_dslw_policy.sta_count = temp_count;
    if(temp_count != 0)
    {
        memcpy(current_policy_cfg->em_config.local_steering_dslw_policy.disallowed_sta, new_policy_cfg->local_steering_dslw_policy.disallowed_sta, temp_count * sizeof(mac_addr_t));
    }

    temp_count = new_policy_cfg->radio_metrics_policies.radio_count;
    current_policy_cfg->em_config.radio_metrics_policies.radio_count = temp_count;
    if(temp_count != 0)
    {
        memcpy(current_policy_cfg->em_config.radio_metrics_policies.radio_metrics_policy, new_policy_cfg->radio_metrics_policies.radio_metrics_policy, temp_count * sizeof(radio_metrics_policy_t));
    }

    push_em_config_event_to_monitor_queue(app, mon_stats_request_state_start);

    return RETURN_OK;
}

int em_event(wifi_app_t *app, wifi_event_t *event)
{
    switch (event->event_type) {
        case wifi_event_type_webconfig:
            handle_em_webconfig_event(app, event);
        break;
        case wifi_event_type_monitor:
            monitor_event_em(app, event);
        break;
        default:
        break;
    }
    return RETURN_OK;
}

int em_init(wifi_app_t *app, unsigned int create_flag)
{
    int rc = RETURN_OK;
    char *component_name = "WifiEM";
    int num_elements;
    em_config_t *policy_config = &app->data.u.em_data.em_config;

    policy_config->btm_steering_dslw_policy.sta_count = 0;
    policy_config->local_steering_dslw_policy.sta_count = 0;
    policy_config->radio_metrics_policies.radio_count = 0;

    if (app_init(app, create_flag) != 0) {
        return RETURN_ERR;
    }

    wifi_util_info_print(WIFI_EM, "%s:%d: Init em app %s\n", __func__, __LINE__, rc ? "failure" : "success");

    return rc;
}

int em_deinit(wifi_app_t *app)
{
    return RETURN_OK;
}