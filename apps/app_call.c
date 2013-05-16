#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libconfig.h>
#include "app.h"
#include "session.h"
#include "manager.h"
#include "filter.h"
#include "log.h"


struct app_call_config
{
    char incontext[80];
    char outcontext[80];
    char rol[20];
    int autoanswer;
} call_config;

struct app_call_info
{
    char actionid[20];
    char ouid[50];
    char duid[50];
};

int
call_state(filter_t *filter, ami_message_t *msg)
{
    struct app_call_info *info = (struct app_call_info *) filter_get_userdata(filter);
    const char *event = message_get_header(msg, "Event");
    const char *from, *to;

    // So this leg is ?
    if (!strcasecmp(message_get_header(msg, "UniqueID"), info->ouid)) {
        from = "AGENT";
        to = "REMOTE";
    } else {
        from = "REMOTE";
        to = "AGENT";
    }

    // Send CallStatus message depending on received event
    if (!strcasecmp(event, "Hangup")) {
        // Print status message dpending on Hangup Cause
        const char *cause = message_get_header(msg, "Cause");
        if (!strcasecmp(cause, "0") || !strcasecmp(cause, "21")) {
            session_write(filter->sess, "CALLSTATUS %s %s ERROR\n", info->actionid, from);
        } else if (!strcasecmp(cause, "16")) {
            session_write(filter->sess, "CALLSTATUS %s %s HANGUP\n", info->actionid, from);
        } else if (!strcasecmp(cause, "17")) {
            session_write(filter->sess, "CALLSTATUS %s %s BUSY\n", info->actionid, from);
        } else {
            session_write(filter->sess, "CALLSTATUS %s %s UNKOWNHANGUP %s\n", info->actionid, from, cause);
	}

        // We dont expect more info about this filter, it's safe to unregister it here
        filter_unregister(filter);

    } else if (!strcasecmp(event, "Newstate")) {
        // Print status message depending on Channel Status
        const char *state = message_get_header(msg, "ChannelState");
        if (!strcasecmp(state, "5")) {
            session_write(filter->sess, "CALLSTATUS %s %s RINGING\n", info->actionid, from);
        } else if (!strcasecmp(state, "6")) {
            session_write(filter->sess, "CALLSTATUS %s %s ANSWERED\n", info->actionid, from);
        }
    } else if (!strcasecmp(event, "VarSet")) {
        const char *value = message_get_header(msg, "Value");
        if (!strcasecmp(value, "SIP 183 Session Progress")) {
            session_write(filter->sess, "CALLSTATUS %s %s PROGRESS\n", info->actionid, to);
        }
    } else if (!strcasecmp(event, "Dial") && !strcasecmp(message_get_header(msg, "SubEvent"),
            "Begin")) {
        // Get the UniqueId from the agent channel
        strcpy(info->duid, message_get_header(msg, "DestUniqueID"));

        // Register a Filter for the agent status
        filter_t *remotefilter = filter_create(filter->sess, FILTER_SYNC_CALLBACK, call_state);
        remotefilter->app_info = info;
        filter_add_condition2(remotefilter, MATCH_EXACT, "UniqueID", info->duid);
        filter_register(remotefilter);

        // Say we have the remote channel
        session_write(filter->sess, "CALLSTATUS %s REMOTE STARTING\n", info->actionid);
    }
}

int
call_response(filter_t *filter, ami_message_t *msg)
{
    struct app_call_info *info = (struct app_call_info *) filter_get_userdata(filter);

    // Get the UniqueId from the agent channel
    strcpy(info->ouid, message_get_header(msg, "UniqueID"));

    // Register a Filter for the agent status
    filter_t *agentfilter = filter_create(filter->sess, FILTER_SYNC_CALLBACK, call_state);
    filter_add_condition2(agentfilter, MATCH_EXACT, "UniqueID", info->ouid);
    filter_set_userdata(agentfilter, (void*) info);
    filter_register(agentfilter);

    // Tell the client the channel is going on!
    session_write(filter->sess, "CALLSTATUS %s AGENT STARTING\n", info->actionid);

    // Remove this filter, we have the uniqueID
    filter_unregister(filter);
}

int
call_exec(session_t *sess, const char *args)
{
    char actionid[20];
    char exten[20];

    if (!session_test_flag(sess, SESS_FLAG_AUTHENTICATED)) {
        return NOT_AUTHENTICATED;
    }

    // Get Call parameteres
    if (sscanf(args, "%s %s", actionid, exten) != 2) {
        return INVALID_ARGUMENTS;
    }

    // Initialize application info
    struct app_call_info *info = malloc(sizeof(struct app_call_info));
    memset(info->ouid, 0, sizeof(info->ouid));
    memset(info->duid, 0, sizeof(info->ouid));
    strcpy(info->actionid, actionid);

    // Register a Filter to get Generated Channel
    filter_t *channelfilter = filter_create(sess, FILTER_SYNC_CALLBACK, call_response);
    filter_add_condition2(channelfilter, MATCH_EXACT, "Event", "VarSet");
    filter_add_condition2(channelfilter, MATCH_EXACT, "Variable", "ACTIONID");
    filter_add_condition2(channelfilter, MATCH_EXACT, "Value", actionid);
    filter_set_userdata(channelfilter, (void*) info);
    filter_register(channelfilter);

    // Get the logged agent
    const char *agent = session_get_variable(sess, "AGENT");

    // Construct a Request message
    ami_message_t msg;
    memset(&msg, 0, sizeof(ami_message_t));
    message_add_header(&msg, "Action: Originate");
    message_add_header(&msg, "CallerID: %s", agent);
    message_add_header(&msg, "Channel: Local/%s@%s", agent, call_config.incontext);
    message_add_header(&msg, "Context: %s", call_config.outcontext);
    message_add_header(&msg, "Priority: 1");
    message_add_header(&msg, "ActionID: %s", actionid);
    message_add_header(&msg, "Exten: %s", exten);
    message_add_header(&msg, "Async: 1");
    message_add_header(&msg, "Variable: ACTIONID=%s", actionid);
    message_add_header(&msg, "Variable: ROL=%s", call_config.rol);
    message_add_header(&msg, "Variable: CALLERID=%s", agent);
    message_add_header(&msg, "Variable: AUTOANSWER=%d", call_config.autoanswer);
    manager_write_message(manager, &msg);

    return 0;
}

int
read_call_config(const char *cfile)
{
    config_t cfg;
    // Initialize configuration
    config_init(&cfg);
    const char *value;
    long int intvalue;

    // Read configuraiton file
    if (config_read_file(&cfg, cfile) == CONFIG_FALSE) {
        isaac_log(LOG_ERROR, "Error parsing configuration file %s on line %d: %s\n", cfile,
                config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return -1;
    }

    // Read known configuration options
    if (config_lookup_string(&cfg, "originate.incontext", &value) == CONFIG_TRUE){
        strcpy(call_config.incontext, value);
    }
    if (config_lookup_string(&cfg, "originate.outcontext", &value) == CONFIG_TRUE){
        strcpy(call_config.outcontext, value);
    }
    if (config_lookup_string(&cfg, "originate.rol", &value) == CONFIG_TRUE){
        strcpy(call_config.rol, value);
    }
    if (config_lookup_int(&cfg, "originate.autoanswer", &intvalue) == CONFIG_TRUE){
        call_config.autoanswer = intvalue;
    }

    return 0;
}

int
load_module()
{
    if (read_call_config(CALLCONF) != 0) {
        isaac_log(LOG_ERROR, "Failed to read app_call config file %s\n", CALLCONF);
        return -1;
    }
    return application_register("Call", call_exec);
}

int
unload_module()
{
    return application_unregister("Call");
}
