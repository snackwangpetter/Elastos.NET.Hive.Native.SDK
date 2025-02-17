/*
 * Copyright (c) 2019 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#include <crystal.h>

#include "hive_error.h"
#include "hive_client.h"
#include "native_client.h"
#include "ipfs_client.h"
#include "onedrive.h"
#include "owncloud.h"

typedef struct FactoryMethod {
    int drive_type;
    HiveClient *(*create_client)(const HiveOptions *);
} FactoryMethod;

static FactoryMethod factory_methods[] = {
    { HiveDriveType_Native,    native_client_new   },
    { HiveDriveType_IPFS,      ipfs_client_new     },
    { HiveDriveType_OneDrive,  onedrive_client_new },
    { HiveDriveType_ownCloud,  owncloud_client_new },
    { HiveDriveType_Butt,      NULL }
};

void ela_log_init(ElaLogLevel level, const char *log_file,
                  void (*log_printer)(const char *format, va_list args))
{
    vlog_init(level, log_file, log_printer);
}

HiveClient *hive_client_new(const HiveOptions *options)
{
    FactoryMethod *method = &factory_methods[0];
    HiveClient *client;
    struct stat st;
    int rc;

    if (!options || !options->persistent_location ||
        !*options->persistent_location) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return NULL;
    }

    rc = stat(options->persistent_location, &st);
    if (rc < 0|| !S_ISDIR(st.st_mode)) {
        vlogE("Client: failed to call stat() (%d).", errno);
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return NULL;
    }

    for (; method->create_client; method++) {
        if (method->drive_type == options->drive_type) {
            client = method->create_client(options);
            break;
        }
    }

    if (!method->create_client) {
        vlogE("Client: unsupported client type.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_SUPPORTED));
        return NULL;
    }

    return client;
}

int hive_client_close(HiveClient *client)
{
    if (!client)
        return 0;

    if (client->close)
        client->close(client);

    return 0;
}

int hive_client_login(HiveClient *client,
                      HiveRequestAuthenticationCallback *callback,
                      void *context)
{
    int rc;

    if (!client) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return -1;
    }

    /*
     * Check login state.
     * 1. If already logined, return OK immediately, else
     * 2. if being in progress of logining, then return error. else
     * 3. It's in raw state, conduct the login process.
     */
    rc = check_and_login(client);
    switch(rc) {
    case 0:
        break;

    case 1:
        vlogD("Hive: This client logined already");
        return 0;

    case -1:
    default:
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_WRONG_STATE));
        return -1;
    }

    if (client->login) {
        rc = client->login(client, callback, context);
        if (rc < 0) {
            vlogE("Client: Failed to login.");
            // recover back to 'RAW' state.
            _test_and_swap(&client->state, LOGINING, RAW);
            hive_set_error(rc);
            return -1;
        }
    }

    // When conducting all login stuffs successfully, then change to be
    // 'LOGINED'.
    _test_and_swap(&client->state, LOGINING, LOGINED);
    return 0;
}

int hive_client_logout(HiveClient *client)
{
    int rc;

    if (!client) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return -1;
    }

    rc = check_and_logout(client);
    switch(rc) {
    case 0:
        break;

    case 1:
        return 0;

    case -1:
    default:
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_WRONG_STATE));
        return -1;
    }

    if (client->logout)
        client->logout(client);

    _test_and_swap(&client->state, LOGOUTING, RAW);

    return 0;
}

int hive_client_get_info(HiveClient *client, HiveClientInfo *info)
{
    int rc;

    if (!client || !info) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return -1;
    }

    if (!has_valid_token(client))  {
        vlogE("Client: client not in valid state.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_READY));
        return -1;
    }

    if (!client->get_info) {
        vlogE("Client: client type does not support this method.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_SUPPORTED));
        return -1;
    }

    rc = client->get_info(client, info);
    if (rc < 0) {
        hive_set_error(rc);
        return -1;
    }

    return 0;
}

HiveDrive *hive_drive_open(HiveClient *client)
{
    HiveDrive *drive;
    int rc;

    if (!client) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return NULL;
    }

    if (!has_valid_token(client))  {
        vlogE("Client: client is not in valid state.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_READY));
        return NULL;
    }

    if (!client->get_drive) {
        vlogE("Client: client type does not support this method.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_SUPPORTED));
        return NULL;
    }

    rc = client->get_drive(client, &drive);
    if (rc < 0) {
        vlogE("Client: Failed to open drive.");
        hive_set_error(rc);
        return NULL;
    }

    return drive;
}

int hive_set_access_token_expired(HiveClient *client) {
    int rc;

    if (!client) {
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_INVALID_ARGS));
        return -1;
    }

    if (!has_valid_token(client)) {
        vlogE("Client: client not in valid state.");
        hive_set_error(HIVE_GENERAL_ERROR(HIVEERR_NOT_READY));
        return -1;
    }

    if (!client->expire_token)
        return 0;

    rc = client->expire_token(client);
    if (rc < 0) {
        hive_set_error(rc);
        return -1;
    }

    return 0;
}
