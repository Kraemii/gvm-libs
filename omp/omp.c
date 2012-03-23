/* openvase-libraries/omp
 * $Id$
 * Description: OMP client interface.
 *
 * Authors:
 * Matthew Mundell <matt@mundell.ukfsn.org>
 *
 * Copyright:
 * Copyright (C) 2009 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * or, at your option, any later version as published by the Free
 * Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @todo Name functions consistently (perhaps omp_*). */

/**
 * @file omp.c
 * @brief OMP client interface.
 *
 * This provides higher level, OMP-aware, facilities for working with with
 * the OpenVAS manager.
 *
 * There are examples of using this interface in the openvas-manager tests.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#define sleep Sleep
#endif

#include <errno.h>

#include "omp.h"
#include "xml.h"
#include "openvas_server.h"

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "lib   omp"


/* Local XML interface extension. */

/** @todo Use next_entities and first_entity instead of this. */

/**
 * @brief Do something for each child of an entity.
 *
 * Calling "break" during body exits the loop.
 *
 * @param[in]  entity  The entity.
 * @param[in]  child   Name to use for child variable.
 * @param[in]  temp    Name to use for internal variable.
 * @param[in]  body    The code to run for each child.
 */
#define DO_CHILDREN(entity, child, temp, body)      \
  do                                                \
    {                                               \
      GSList* temp = entity->entities;              \
      while (temp)                                  \
        {                                           \
          entity_t child = temp->data;              \
          {                                         \
            body;                                   \
          }                                         \
          temp = g_slist_next (temp);               \
        }                                           \
    }                                               \
  while (0)

#if 0
/* Lisp version of DO_CHILDREN. */
(defmacro do-children ((entity child) &body body)
  "Do something for each child of an entity."
  (let ((temp (gensym)))
    `(while ((,temp (entity-entities ,entity) (rest ,temp)))
            (,temp)
       ,@body)))
#endif


/* OMP. */

/**
 * @brief Get the task status from an OMP GET_TASKS response.
 *
 * @param[in]  response   GET_TASKS response.
 *
 * @return The entity_text of the status entity if the entity is found, else
 *         NULL.
 */
const char*
omp_task_status (entity_t response)
{
  entity_t task = entity_child (response, "task");
  if (task)
    {
      entity_t status = entity_child (task, "status");
      if (status) return entity_text (status);
    }
  return NULL;
}

/**
 * @brief "Ping" the manager.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  timeout   Server idle time before giving up, in milliseconds.  0
 *                       to wait forever.
 *
 * @return 0 on success, 1 if manager closed connection, 2 on timeout,
 *         -1 on error.
 */
int
omp_ping (gnutls_session_t *session, int timeout)
{
  entity_t entity;
  const char* status;
  char first;
  int ret;

  /* Send a GET_VERSION request. */

  ret = openvas_server_send (session, "<get_version/>");
  if (ret)
    return ret;

  /* Read the response, with a timeout. */

  entity = NULL;
  switch (try_read_entity (session, timeout, &entity))
    {
      case 0:
        break;
      case -4:
        return 2;
      default:
        return -1;
    }

  /* Check the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  first = status[0];
  free_entity (entity);
  if (first == '2') return 0;
  return -1;
}

/**
 * @brief Authenticate with the manager.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  username  Username.
 * @param[in]  password  Password.
 *
 * @return 0 on success, 1 if manager closed connection, 2 if auth failed,
 *         -1 on error.
 */
int
omp_authenticate (gnutls_session_t* session,
                  const char* username,
                  const char* password)
{
  entity_t entity;
  const char* status;
  char first;
  gchar* msg;

  /* Send the auth request. */

  msg = g_markup_printf_escaped ("<authenticate><credentials>"
                                 "<username>%s</username>"
                                 "<password>%s</password>"
                                 "</credentials></authenticate>",
                                 username,
                                 password);
  int ret = openvas_server_send (session, msg);
  g_free (msg);
  if (ret) return ret;

  /* Read the response. */

  entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  first = status[0];
  free_entity (entity);
  if (first == '2') return 0;
  return 2;
}

/**
 * @brief Authenticate with the manager.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  username  Username.
 * @param[in]  password  Password.
 * @param[out] role      Role.
 * @param[out] timezone  Timezone if any, else NULL.
 *
 * @return 0 on success, 1 if manager closed connection, 2 if auth failed,
 *         -1 on error.
 */
int
omp_authenticate_info (gnutls_session_t *session,
                       const char *username,
                       const char *password,
                       char **role,
                       char **timezone)
{
  entity_t entity;
  const char* status;
  char first;
  gchar* msg;

  *timezone = NULL;

  /* Send the auth request. */

  msg = g_markup_printf_escaped ("<authenticate><credentials>"
                                 "<username>%s</username>"
                                 "<password>%s</password>"
                                 "</credentials></authenticate>",
                                 username,
                                 password);
  int ret = openvas_server_send (session, msg);
  g_free (msg);
  if (ret) return ret;

  /* Read the response. */

  entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  first = status[0];
  if (first == '2')
    {
      entity_t timezone_entity, role_entity;
      /* Get the extra info. */
      timezone_entity = entity_child (entity, "timezone");
      if (timezone_entity)
        *timezone = g_strdup (entity_text (timezone_entity));
      role_entity = entity_child (entity, "role");
      if (role_entity)
        *role = g_strdup (entity_text (role_entity));
      free_entity (entity);
      return 0;
    }
  free_entity (entity);
  return 2;
}

/**
 * @brief Authenticate, getting credentials from the environment.
 *
 * Get the user name from environment variable OPENVAS_TEST_USER if that is
 * set, else from USER.  Get the password from OPENVAS_TEST_PASSWORD.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 *
 * @return 0 on success, 1 if manager closed connection, -1 on error.
 */
int
omp_authenticate_env (gnutls_session_t* session)
{
  char* user = getenv ("OPENVAS_TEST_USER");
  if (user == NULL)
    {
      user = getenv ("USER");
      if (user == NULL) return -1;
    }

  char* password = getenv ("OPENVAS_TEST_PASSWORD");
  if (password == NULL) return -1;

  return omp_authenticate (session, user, password);
}

/**
 * @brief Create a task given a config and target.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Task name.
 * @param[in]   config      Task config name.
 * @param[in]   target      Task target name.
 * @param[in]   comment     Task comment.
 * @param[out]  id          Pointer for newly allocated ID of new task.  Only
 *                          set on successful return.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_create_task (gnutls_session_t* session,
                 const char* name,
                 const char* config,
                 const char* target,
                 const char* comment,
                 char** id)
{
  /* Create the OMP request. */

  gchar* new_task_request;
  new_task_request = g_markup_printf_escaped ("<create_task>"
                                              "<config id=\"%s\"/>"
                                              "<target id=\"%s\"/>"
                                              "<name>%s</name>"
                                              "<comment>%s</comment>"
                                              "</create_task>",
                                              config,
                                              target,
                                              name,
                                              comment);

  /* Send the request. */

  int ret = openvas_server_send (session, new_task_request);
  g_free (new_task_request);
  if (ret) return -1;

  /* Read the response. */

  ret = omp_read_create_response (session, id);
  if (ret == 201)
    return 0;
  return ret;
}

/**
 * @brief Create a task, given the task description as an RC file.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   config      Task configuration.
 * @param[in]   config_len  Length of config.
 * @param[in]   name        Task name.
 * @param[in]   comment     Task comment.
 * @param[out]  id          Pointer for newly allocated ID of new task.  Only
 *                          set on successful return.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_create_task_rc (gnutls_session_t* session,
                    const char* config,
                    unsigned int config_len,
                    const char* name,
                    const char* comment,
                    char** id)
{
  /* Convert the file contents to base64. */

  gchar* new_task_file = strlen (config)
                         ? g_base64_encode ((guchar*) config, config_len)
                         : g_strdup ("");

  /* Create the OMP request. */

  gchar* new_task_request;
  new_task_request = g_markup_printf_escaped ("<create_task>"
                                              "<rcfile>%s</rcfile>"
                                              "<name>%s</name>"
                                              "<comment>%s</comment>"
                                              "</create_task>",
                                              new_task_file,
                                              name,
                                              comment);
  g_free (new_task_file);

  /* Send the request. */

  int ret = openvas_server_send (session, new_task_request);
  g_free (new_task_request);
  if (ret) return -1;

  /* Read the response. */

  entity_t entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Get the ID of the new task from the response. */

  entity_t id_entity = entity_child (entity, "task_id");
  if (id_entity == NULL)
    {
      free_entity (entity);
      return -1;
    }
  *id = g_strdup (entity_text (id_entity));
  return 0;
}

/**
 * @brief Create a task, given the task description as an RC file.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   file_name   Name of the RC file.
 * @param[in]   name        Task name.
 * @param[in]   comment     Task comment.
 * @param[out]  id          ID of new task.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_create_task_rc_file (gnutls_session_t* session,
                         const char* file_name,
                         const char* name,
                         const char* comment,
                         char** id)
{
  gchar* new_task_rc = NULL;
  gsize new_task_rc_len;
  GError* error = NULL;

  /* Read in the RC file. */

  g_file_get_contents (file_name,
                       &new_task_rc,
                       &new_task_rc_len,
                       &error);
  if (error)
    {
      g_error_free (error);
      return -1;
    }

  int ret = omp_create_task_rc (session,
                                new_task_rc,
                                new_task_rc_len,
                                name,
                                comment,
                                id);
  g_free (new_task_rc);
  return ret;
}

/**
 * @brief Start a task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 * @param[out]  report_id  ID of report.
 *
 * @return 0 on success, 1 on failure, -1 on error.
 */
int
omp_start_task_report (gnutls_session_t* session, const char* task_id,
                       char** report_id)
{
  if (openvas_server_sendf (session,
                            "<start_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  /* Read the response. */

  entity_t entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  const char* status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  char first = status[0];
  if (first == '2')
    {
      if (report_id)
        {
          entity_t report_id_xml = entity_child (entity, "report_id");
          if (report_id_xml)
            *report_id = g_strdup (entity_text (report_id_xml));
          else
            {
              free_entity (entity);
              return -1;
            }
        }
      free_entity (entity);
      return 0;
    }
  free_entity (entity);
  return 1;
}

/**
 * @brief Start a task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 *
 * @return 0 on success, 1 on failure, -1 on error.
 */
int
omp_start_task (gnutls_session_t* session, const char* task_id)
{
  return omp_start_task_report (session, task_id, NULL);
}

/**
 * @brief Resume or start a task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 * @param[out]  report_id  ID of report.
 *
 * @return 0 on success, 1 on failure, -1 on error.
 */
int
omp_resume_or_start_task_report (gnutls_session_t* session, const char* task_id,
                                 char** report_id)
{
  if (openvas_server_sendf (session,
                            "<resume_or_start_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  /* Read the response. */

  entity_t entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  const char* status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  char first = status[0];
  if (first == '2')
    {
      if (report_id)
        {
          entity_t report_id_xml = entity_child (entity, "report_id");
          if (report_id_xml)
            *report_id = g_strdup (entity_text (report_id_xml));
          else
            {
              free_entity (entity);
              return -1;
            }
        }
      free_entity (entity);
      return 0;
    }
  free_entity (entity);
  return 1;
}

/**
 * @brief Resume or start a task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 *
 * @return 0 on success, 1 on failure, -1 on error.
 */
int
omp_resume_or_start_task (gnutls_session_t* session, const char* task_id)
{
  return omp_resume_or_start_task_report (session, task_id, NULL);
}

/** @todo Use this in the other functions. */
/**
 * @brief Read response and convert status of response to a return value.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 *
 * @return 0 on success, 1 on failure, -1 on error.
 */
int
check_response (gnutls_session_t* session)
{
  char first;
  const char* status;
  entity_t entity;

  /* Read the response. */

  entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  first = status[0];
  free_entity (entity);
  if (first == '2') return 0;
  return 1;
}

/**
 * @brief Read response status and resource UUID.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[out] uuid     Either NULL or address for freshly allocated UUID of
 *                      created response.
 *
 * @return OMP response code on success, -1 on error.
 */
int
omp_read_create_response (gnutls_session_t* session, char **uuid)
{
  int ret;
  const char *status, *id;
  entity_t entity;

  /* Read the response. */

  entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Parse the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }

  if (uuid)
    {
      id = entity_attribute (entity, "id");
      if (id == NULL)
        {
          free_entity (entity);
          return -1;
        }
      if (strlen (id) == 0)
        {
          free_entity (entity);
          return -1;
        }
      *uuid = g_strdup (id);
    }

  ret = atoi (status);
  free_entity (entity);
  return ret;
}

/**
 * @brief Deprecated wrapper function for /ref omp_stop_task.
 *
 * @deprecated Use /ref omp_stop_task instead.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_abort_task (gnutls_session_t* session, const char* id)
{
  return omp_stop_task (session, id);
}

/**
 * @brief Stop a task and read the manager response.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_stop_task (gnutls_session_t* session, const char* id)
{
  if (openvas_server_sendf (session,
                            "<stop_task task_id=\"%s\"/>",
                            id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Pause a task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 *
 * @return 0 on success, 1 on OMP failure, -1 on error.
 */
int
omp_pause_task (gnutls_session_t* session, const char* task_id)
{
  if (openvas_server_sendf (session,
                            "<pause_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Resume a paused task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 *
 * @return 0 on success, 1 on OMP failure, -1 on error.
 */
int
omp_resume_paused_task (gnutls_session_t* session, const char* task_id)
{
  if (openvas_server_sendf (session,
                            "<resume_paused_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Resume a stopped task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 *
 * @return 0 on success, 1 on OMP failure, -1 on error.
 */
int
omp_resume_stopped_task (gnutls_session_t* session, const char* task_id)
{
  if (openvas_server_sendf (session,
                            "<resume_stopped_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Resume a stopped task and read the manager response.
 *
 * @param[in]   session    Pointer to GNUTLS session.
 * @param[in]   task_id    ID of task.
 * @param[out]  report_id  ID of report.
 *
 * @return 0 on success, 1 on OMP failure, -1 on error.
 */
int
omp_resume_stopped_task_report (gnutls_session_t* session, const char* task_id,
                                char** report_id)
{
  if (openvas_server_sendf (session,
                            "<resume_stopped_task task_id=\"%s\"/>",
                            task_id)
      == -1)
    return -1;

  /* Read the response. */

  entity_t entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  const char* status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  char first = status[0];
  if (first == '2')
    {
      if (report_id)
        {
          entity_t report_id_xml = entity_child (entity, "report_id");
          if (report_id_xml)
            *report_id = g_strdup (entity_text (report_id_xml));
          else
            {
              free_entity (entity);
              return -1;
            }
        }
      free_entity (entity);
      return 0;
    }
  free_entity (entity);
  return 1;
}

/**
 * @brief Issue \ref command against the server in \ref session, waits for
 * @brief the response and fills the response entity into \ref response.
 *
 * @param[in]  session   Pointer to GnuTLS session to an omp server.
 * @param[in]  command   Command to issue against the server.
 * @param[out] response  Entity holding the response, if any.
 *
 * @return 0 in case of success. -1 otherwise (e.g. invalid session).
 */
static int
get_omp_response_503 (gnutls_session_t* session, const gchar* command,
                      entity_t* response)
{
  while (1)
    {
      const char* status;

      if (openvas_server_send (session, command))
        return -1;

      *response = NULL;
      if (read_entity (session, response)) return -1;

      status = entity_attribute (*response, "status");
      if (status == NULL)
        {
          free_entity (*response);
          return -1;
        }
      if (strlen (status) == 0)
        {
          free_entity (*response);
          return -1;
        }
      char first = status[0];
      if (first == '2') return 0;
      if (strlen (status) == 3 && strcmp (status, "503") == 0)
        {
          /** @todo evaluate if response has to be freed here */
          sleep (0.5);
          continue;
        }
      free_entity (*response);
      return -1;
    }
}

/**
 * @brief Issue an OMP \<get_nvts\/\> command and wait for the response.
 *
 * @param[in]  session   Session to the server.
 * @param[out] response  Entity containing the response, must be freed.
 *
 * @return 0 in case of success. -1 otherwise (e.g. invalid session).
 */
int
omp_get_nvt_all (gnutls_session_t* session, entity_t* response)
{
  return get_omp_response_503 (session, "<get_nvts details=\"0\"/>", response);
}

/**
 * @brief Issue an OMP \<get_nvt_feed_checksum algoithm=md5/\> command and
 * @brief wait for the response.
 *
 * @param[in]  session   Session to the server.
 * @param[out] response  Entity containing the response, must be freed.
 *
 * @return 0 in case of success. -1 otherwise (e.g. invalid session).
 */
int
omp_get_nvt_feed_checksum (gnutls_session_t* session, entity_t* response)
{
  return get_omp_response_503 (session,
                               "<get_nvt_feed_checksum algorithm=\"md5\"/>",
                               response);
}


/**
 * @brief Issue an OMP \<get_dependencies/\> command and wait for the response.
 *
 * @param[in]  session   Session to the server.
 * @param[out] response  Entity containing the response, must be freed.
 *
 * @return 0 in case of success. -1 otherwise (e.g. invalid session).
 */
int
omp_get_dependencies_503 (gnutls_session_t* session, entity_t* response)
{
  return get_omp_response_503 (session, "<get_dependencies/>", response);
}


/**
 * @brief Wait for a task to start running on the server.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, 1 on internal error in task, -1 on error.
 */
int
omp_wait_for_task_start (gnutls_session_t* session,
                         const char* id)
{
  while (1)
    {
      if (openvas_server_sendf (session, "<get_tasks/>") == -1)
        return -1;

      /* Read the response. */

      entity_t entity = NULL;
      if (read_entity (session, &entity)) return -1;

      /* Check the response. */

      const char* status = entity_attribute (entity, "status");
      if (status == NULL)
        {
          free_entity (entity);
          return -1;
        }
      if (strlen (status) == 0)
        {
          free_entity (entity);
          return -1;
        }
      if (status[0] == '2')
        {
          /* Check the running status of the given task. */

          char* run_state = NULL;

#if 0
          /* Lisp version. */
          (do-children (entity child)
            (when (string= (entity-type child) "task")
              (let ((task-id (entity-attribute child "task_id")))
                (fi* task-id
                  (free-entity entity)
                  (return-from wait-for-task-start -1))
                (when (string= task-id id)
                  (let ((status (entity-child child "status")))
                    (fi* status
                      (free-entity entity)
                      (return-from wait-for-task-start -1))
                    (setq run-state (entity-text status)))
                  (return)))))
#endif

          DO_CHILDREN (entity, child, temp,
                       if (strcasecmp (entity_name (child), "task") == 0)
                         {
                           const char* task_id = entity_attribute (child, "id");
                           if (task_id == NULL)
                             {
                               free_entity (entity);
                               return -1;
                             }
                           if (strcasecmp (task_id, id) == 0)
                             {
                               entity_t status = entity_child (child, "status");
                               if (status == NULL)
                                 {
                                   free_entity (entity);
                                   return -1;
                                 }
                               run_state = entity_text (status);
                               break;
                             }
                         });

          if (run_state == NULL)
            {
              free_entity (entity);
              return -1;
            }

          if (strcmp (run_state, "Running") == 0
              || strcmp (run_state, "Done") == 0)
            {
              free_entity (entity);
              return 0;
            }
          if (strcmp (run_state, "Internal Error") == 0)
            {
              free_entity (entity);
              return 1;
            }
          free_entity (entity);
        }

      /** @todo Reconsider this (more below). */
      sleep (1);
    }
}

/**
 * @brief Wait for a task to finish running on the server.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, 1 on internal error in task, -1 on error.
 */
int
omp_wait_for_task_end (gnutls_session_t* session, const char* id)
{
  while (1)
    {
      if (openvas_server_sendf (session, "<get_tasks/>") == -1)
        return -1;

      /* Read the response. */

      entity_t entity = NULL;
      if (read_entity (session, &entity)) return -1;

      /* Check the response. */

      const char* status = entity_attribute (entity, "status");
      if (status == NULL)
        {
          free_entity (entity);
          return -1;
        }
      if (strlen (status) == 0)
        {
          free_entity (entity);
          return -1;
        }
      if (status[0] == '2')
        {
          /* Check the running status of the given task. */

          char* run_state = NULL;

#if 0
          /* Lisp version. */
          (do-children (entity child)
            (when (string= (entity-type child) "task")
              (let ((task-id (entity-attribute child "task_id")))
                (fi* task-id
                  (free-entity entity)
                  (return-from wait-for-task-start -1))
                (when (string= task-id id)
                  (let ((status (entity-child child "status")))
                    (fi* status
                      (free-entity entity)
                      (return-from wait-for-task-start -1))
                    (setq run-state (entity-text status)))
                  (return)))))
#endif

          DO_CHILDREN (entity, child, temp,
                       if (strcasecmp (entity_name (child), "task") == 0)
                         {
                           const char* task_id = entity_attribute (child, "id");
                           if (task_id == NULL)
                             {
                               free_entity (entity);
                               return -1;
                             }
                           if (strcasecmp (task_id, id) == 0)
                             {
                               entity_t status = entity_child (child, "status");
                               if (status == NULL)
                                 {
                                   free_entity (entity);
                                   return -1;
                                 }
                               run_state = entity_text (status);
                               break;
                             }
                         });

          if (run_state == NULL)
            {
              free_entity (entity);
              return -1;
            }

          if (strcmp (run_state, "Done") == 0)
            {
              free_entity (entity);
              return 0;
            }
          if (strcmp (run_state, "Internal Error") == 0)
            {
              free_entity (entity);
              return 1;
            }
          if (strcmp (run_state, "Stopped") == 0)
            {
              free_entity (entity);
              return 1;
            }
          free_entity (entity);
        }

      sleep (1);
    }
}

/**
 * @brief Wait for a task to stop on the server.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, 1 on internal error in task, -1 on error,
 *         -2 on failure to find the task.
 */
int
omp_wait_for_task_stop (gnutls_session_t* session, const char* id)
{
  while (1)
    {
      if (openvas_server_sendf (session, "<get_tasks/>") == -1)
        return -1;

      /* Read the response. */

      entity_t entity = NULL;
      if (read_entity (session, &entity)) return -1;

      /* Check the response. */

      const char* status = entity_attribute (entity, "status");
      if (status == NULL)
        {
          free_entity (entity);
          return -1;
        }
      if (strlen (status) == 0)
        {
          free_entity (entity);
          return -1;
        }
      if (status[0] == '2')
        {
          /* Check the running status of the given task. */

          char* run_state = NULL;

#if 0
          /* Lisp version. */
          (do-children (entity child)
            (when (string= (entity-type child) "task")
              (let ((task-id (entity-attribute child "task_id")))
                (fi* task-id
                  (free-entity entity)
                  (return-from wait-for-task-start -1))
                (when (string= task-id id)
                  (let ((status (entity-child child "status")))
                    (fi* status
                      (free-entity entity)
                      (return-from wait-for-task-start -1))
                    (setq run-state (entity-text status)))
                  (return)))))
#endif

          DO_CHILDREN (entity, child, temp,
                       if (strcasecmp (entity_name (child), "task") == 0)
                         {
                           const char* task_id = entity_attribute (child, "id");
                           if (task_id == NULL)
                             {
                               free_entity (entity);
                               return -1;
                             }
                           if (strcasecmp (task_id, id) == 0)
                             {
                               entity_t status = entity_child (child, "status");
                               if (status == NULL)
                                 {
                                   free_entity (entity);
                                   return -1;
                                 }
                               run_state = entity_text (status);
                               break;
                             }
                         });

          if (run_state == NULL)
            {
              free_entity (entity);
              return -2;
            }

          if (strcmp (run_state, "Stopped") == 0)
            {
              free_entity (entity);
              return 0;
            }
          if (strcmp (run_state, "Done") == 0)
            {
              free_entity (entity);
              return 0;
            }
          if (strcmp (run_state, "Internal Error") == 0)
            {
              free_entity (entity);
              return 1;
            }
          free_entity (entity);
        }

      sleep (1);
    }
}

/**
 * @brief Wait for the manager to actually remove a task.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_wait_for_task_delete (gnutls_session_t* session,
                          const char* id)
{
  while (1)
    {
      entity_t entity;
      const char* status;

      if (openvas_server_sendf (session,
                                "<get_tasks task_id=\"%s\"/>",
                                id)
          == -1)
        return -1;

      entity = NULL;
      if (read_entity (session, &entity)) return -1;

      status = omp_task_status (entity);
      if (status == NULL)
        {
          free_entity (entity);
          break;
        }
      free_entity (entity);

      sleep (1);
    }
  return 0;
}

/**
 * @brief Delete a task and read the manager response.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  id       ID of task.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_delete_task (gnutls_session_t* session, const char* id)
{
  if (openvas_server_sendf (session,
                            "<delete_task task_id=\"%s\"/>",
                            id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Deprecated wrapper function for /ref omp_get_tasks.
 *
 * @deprecated Use /ref omp_get_tasks instead.
 *
 * @param[in]  session         Pointer to GNUTLS session.
 * @param[in]  id              ID of task or NULL for all tasks.
 * @param[in]  include_rcfile  Request rcfile in status if true.
 * @param[out] status          Status return.  On success contains GET_TASKS
 *                             response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_status (gnutls_session_t* session, const char* id, int include_rcfile,
                entity_t* status)
{
  return omp_get_tasks (session, id, 1, include_rcfile, status);
}

/**
 * @brief Get the status of a task.
 *
 * @param[in]  session         Pointer to GNUTLS session.
 * @param[in]  id              ID of task or NULL for all tasks.
 * @param[in]  details         Whether to request task details.
 * @param[in]  include_rcfile  Request rcfile in status if true.
 * @param[out] status          Status return.  On success contains GET_TASKS
 *                             response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_tasks (gnutls_session_t* session, const char* id, int details,
               int include_rcfile, entity_t* status)
{
  const char* status_code;
  int ret;

  if (id == NULL)
    {
      if (openvas_server_sendf (session,
                                "<get_tasks details=\"%i\" rcfile=\"%i\"/>",
                                details,
                                include_rcfile)
          == -1)
        return -1;
    }
  else
    {
      if (openvas_server_sendf (session,
                                "<get_tasks"
                                " task_id=\"%s\""
                                " details=\"%i\""
                                " rcfile=\"%i\"/>",
                                id,
                                details,
                                include_rcfile)
          == -1)
        return -1;
    }

  /* Read the response. */

  *status = NULL;
  if (read_entity (session, status)) return -1;

  /* Check the response. */

  status_code = entity_attribute (*status, "status");
  if (status_code == NULL)
    {
      free_entity (*status);
      return -1;
    }
  if (strlen (status_code) == 0)
    {
      free_entity (*status);
      return -1;
    }
  if (status_code[0] == '2') return 0;
  ret = (int) strtol (status_code, NULL, 10);
  free_entity (*status);
  if (errno == ERANGE) return -1;
  return ret;
}

/**
 * @brief Get a target.
 *
 * @param[in]  session         Pointer to GNUTLS session.
 * @param[in]  id              ID of target or NULL for all targets.
 * @param[in]  tasks           Whether to include tasks that use the target.
 * @param[out] target          Target return.  On success contains GET_TARGETS
 *                             response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_targets (gnutls_session_t* session, const char* id, int tasks,
                 int include_rcfile, entity_t* target)
{
  const char* status_code;
  int ret;

  if (id == NULL)
    {
      if (openvas_server_sendf (session,
                                "<get_targets tasks=\"%i\"/>",
                                tasks)
          == -1)
        return -1;
    }
  else
    {
      if (openvas_server_sendf (session,
                                "<get_targets"
                                " target_id=\"%s\""
                                " tasks=\"%i\"/>",
                                id,
                                tasks)
          == -1)
        return -1;
    }

  /* Read the response. */

  *target = NULL;
  if (read_entity (session, target)) return -1;

  /* Check the response. */

  status_code = entity_attribute (*target, "status");
  if (status_code == NULL)
    {
      free_entity (*target);
      return -1;
    }
  if (strlen (status_code) == 0)
    {
      free_entity (*target);
      return -1;
    }
  if (status_code[0] == '2') return 0;
  ret = (int) strtol (status_code, NULL, 10);
  free_entity (*target);
  if (errno == ERANGE) return -1;
  return ret;
}

/**
 * @brief Get a report.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  id        ID of report.
 * @param[out] response  Report.  On success contains GET_REPORT response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_report (gnutls_session_t* session,
                const char* id,
                const char* format,
                int first_result_number,
                entity_t* response)
{
  int ret;
  const char *status_code;

  if (response == NULL)
    return -1;

  if (openvas_server_sendf (session,
                            "<get_reports"
                            " result_hosts_only=\"0\""
                            " first_result=\"%i\""
                            " sort_field=\"ROWID\""
                            " sort_order=\"1\""
                            " format_id=\"%s\""
                            " report_id=\"%s\"/>",
                            first_result_number,
                            format ? format : "XML",
                            id))
    return -1;

  *response = NULL;
  if (read_entity (session, response)) return -1;

  /* Check the response. */

  status_code = entity_attribute (*response, "status");
  if (status_code == NULL)
    {
      free_entity (*response);
      return -1;
    }
  if (strlen (status_code) == 0)
    {
      free_entity (*response);
      return -1;
    }
  if (status_code[0] == '2') return 0;
  ret = (int) strtol (status_code, NULL, 10);
  free_entity (*response);
  if (errno == ERANGE) return -1;
  return ret;
}

/**
 * @brief Get a report in a given format.
 *
 * @todo This now has a bad name, because there is an OMP GET_REPORT_FORMATS.
 *
 * @param[in]  session      Pointer to GNUTLS session.
 * @param[in]  id           ID of report.
 * @param[in]  format       Required format.
 * @param[out] report       Report.  On success contains the report.
 * @param[out] report_size  Size of report in bytes.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_get_report_format (gnutls_session_t* session,
                       const char* id,
                       const char* format,
                       void** report,
                       gsize* report_size)
{
  char first;
  const char* status;
  entity_t entity;

  if (openvas_server_sendf (session,
                            "<get_reports format_id=\"%s\" report_id=\"%s\"/>",
                            format,
                            id))
    return -1;

  /* Read the response. */

  entity = NULL;
  if (read_entity (session, &entity)) return -1;

  /* Check the response. */

  status = entity_attribute (entity, "status");
  if (status == NULL)
    {
      free_entity (entity);
      return -1;
    }
  if (strlen (status) == 0)
    {
      free_entity (entity);
      return -1;
    }
  first = status[0];
  if (first == '2')
    {
      const char* report_64;
      entity_t report_xml;

      report_xml = entity_child (entity, "report");
      if (report_xml == NULL)
        {
          free_entity (entity);
          return -1;
        }

      report_64 = entity_text (report_xml);
      if (strlen (report_64) == 0)
        {
          *report = g_strdup ("");
          *report_size = 0;
        }
      else
        {
          *report = (void*) g_base64_decode (report_64, report_size);
        }

      free_entity (entity);
      return 0;
    }
  free_entity (entity);
  return -1;
}

/**
 * @brief Remove a report.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  id        ID of report.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_delete_report (gnutls_session_t* session, const char* id)
{
  if (openvas_server_sendf (session, "<delete_report report_id=\"%s\"/>", id))
    return -1;

  return check_response (session);
}

/**
 * @brief Get results.
 *
 * @param[in]  session            Pointer to GNUTLS session.
 * @param[in]  task_id            ID of task whose to get, NULL for all.
 * @param[in]  notes              Whether to include notes.
 * @param[in]  notes_details      If notes, whether to include details.
 * @param[in]  overrides          Whether to include notes.
 * @param[in]  overrides_details  If overrides, whether to include details.
 * @param[in]  apply_overrides    Whether to apply overrides.
 * @param[out] response           On success contains the GET_RESULTS response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_results (gnutls_session_t* session,
                 const char* task_id,
                 int notes,
                 int notes_details,
                 int overrides,
                 int overrides_details,
                 int apply_overrides,
                 entity_t* response)
{
  if (openvas_server_sendf (session,
                            "<get_results"
                            "%s%s%s"
                            " notes=\"%i\""
                            " notes_details=\"%i\""
                            " overrides=\"%i\""
                            " overrides_details=\"%i\""
                            " apply_overrides=\"%i\"/>",
                            task_id ? " task_id=\"" : "",
                            task_id ? task_id : "",
                            task_id ? "\"" : "",
                            notes,
                            notes_details,
                            overrides,
                            overrides_details,
                            apply_overrides))
    return -1;

  {
    const char* status;
    entity_t entity;

    /* Read the response. */

    entity = NULL;
    if (read_entity (session, &entity)) return -1;

    /* Check the response. */

    status = entity_attribute (entity, "status");
    if (status == NULL)
      {
        free_entity (entity);
        return -1;
      }
    if (strlen (status) == 0)
      {
        free_entity (entity);
        return -1;
      }
    if (status[0] == '2')
      {
        if (response)
          *response = entity;
        else
          free_entity (entity);
        return 0;
      }
    free_entity (entity);
    return 1;
  }
}

/**
 * @brief Modify a task.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[in]  id        ID of task.
 * @param[in]  rcfile    NULL or new RC file (as plain text).
 * @param[in]  name      NULL or new name.
 * @param[in]  comment   NULL or new comment.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_modify_task (gnutls_session_t* session, const char* id,
                 const char* rcfile, const char* name, const char* comment)
{
  if (openvas_server_sendf (session, "<modify_task task_id=\"%s\">", id))
    return -1;

  if (rcfile)
    {
      if (strlen (rcfile) == 0)
        {
          if (openvas_server_send (session, "<rcfile></rcfile>"))
            return -1;
        }
      else
        {
          gchar *base64_rc = g_base64_encode ((guchar*) rcfile,
                                              strlen (rcfile));
          int ret = openvas_server_sendf (session,
                                          "<rcfile>%s</rcfile>",
                                          base64_rc);
          g_free (base64_rc);
          if (ret) return -1;
        }
    }

  if (name && openvas_server_sendf (session, "<name>%s</name>", name))
    return -1;

  if (comment
      && openvas_server_sendf (session, "<comment>%s</comment>", comment))
    return -1;

  if (openvas_server_send (session, "</modify_task>"))
    return -1;

  return check_response (session);
}

/**
 * @brief Modify a file on a task.
 *
 * @param[in]  session      Pointer to GNUTLS session.
 * @param[in]  id           ID of task.
 * @param[in]  name         Name of file.
 * @param[in]  content      New content.  NULL to remove file.
 * @param[in]  content_len  Length of content.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_modify_task_file (gnutls_session_t* session, const char* id,
                      const char* name, const void* content,
                      gsize content_len)
{
  if (name == NULL) return -1;

  if (openvas_server_sendf (session, "<modify_task task_id=\"%s\">", id))
    return -1;

  if (content)
    {
      if (openvas_server_sendf (session,
                                "<file name=\"%s\" action=\"update\">",
                                name))
        return -1;

      if (content_len)
        {
          gchar *base64_rc = g_base64_encode ((guchar*) content,
                                              content_len);
          int ret = openvas_server_sendf (session,
                                          "%s",
                                          base64_rc);
          g_free (base64_rc);
          if (ret) return -1;
        }

      if (openvas_server_sendf (session, "</file>"))
        return -1;
    }
  else
    {
      if (openvas_server_sendf (session,
                                "<file name=\"%s\" action=\"remove\" />",
                                name))
        return -1;
    }

  if (openvas_server_send (session, "</modify_task>"))
    return -1;

  return check_response (session);
}

/**
 * @brief Get the manager preferences.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[out] response  On success contains GET_PREFERENCES response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_preferences (gnutls_session_t* session, entity_t* response)
{
  if (openvas_server_send (session, "<get_preferences/>"))
    return -1;

  *response = NULL;
  if (read_entity (session, response)) return -1;

  // FIX check status

  return 0;
}

/**
 * @brief Get the manager preferences, waiting for them to appear.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[out] response  On success contains GET_PREFERENCES response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_preferences_503 (gnutls_session_t* session, entity_t* response)
{
  return get_omp_response_503 (session, "<get_preferences/>", response);
}

/**
 * @brief Get the manager certificates.
 *
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[out] response  On success contains GET_CERTIFICATES response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_certificates (gnutls_session_t* session, entity_t* response)
{
  const char* status_code;
  int ret;

  if (openvas_server_send (session, "<get_preferences/>"))
    return -1;

  *response = NULL;
  if (read_entity (session, response)) return -1;

  /* Check the response. */

  status_code = entity_attribute (*response, "status");
  if (status_code == NULL)
    {
      free_entity (*response);
      return -1;
    }
  if (strlen (status_code) == 0)
    {
      free_entity (*response);
      return -1;
    }
  if (status_code[0] == '2') return 0;
  ret = (int) strtol (status_code, NULL, 10);
  free_entity (*response);
  if (errno == ERANGE) return -1;
  return ret;
}

/**
 * @brief Poll an OMP service until it is up.
 *
 * Repeatedly call a function while it returns the value 503.
 *
 * @param[in]  function  Function to call to do polling.
 * @param[in]  session   Pointer to GNUTLS session.
 * @param[out] response  On success contains GET_CERTIFICATES response.
 *
 * @return The value returned from the function.
 */
int
omp_until_up (int (*function) (gnutls_session_t*, entity_t*),
              gnutls_session_t* session,
              entity_t* response)
{
  int ret;
  while ((ret = function (session, response)) == 503);
  return ret;
}

/**
 * @brief Create a target.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Name of target.
 * @param[in]   hosts       Target hosts.
 * @param[in]   comment     Target comment.
 * @param[in]   ssh_credential  UUID of SSH LSC credential.
 * @param[in]   smb_credential  UUID of SMB LSC credential.
 * @param[out]  uuid        Either NULL or address for UUID of created target.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_create_target (gnutls_session_t* session,
                   const char* name,
                   const char* hosts,
                   const char* comment,
                   const char* ssh_credential,
                   const char* smb_credential,
                   char** uuid)
{
  int ret;

  if (comment)
    {
      if (ssh_credential && smb_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<comment>%s</comment>"
                                        "<ssh_lsc_credential id=\"%s\"/>"
                                        "<smb_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        comment,
                                        ssh_credential,
                                        smb_credential);
      else if (ssh_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<comment>%s</comment>"
                                        "<ssh_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        comment,
                                        ssh_credential);
      else if (smb_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<comment>%s</comment>"
                                        "<smb_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        comment,
                                        smb_credential);
      else
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<comment>%s</comment>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        comment);
    }
  else
    {
      if (ssh_credential && smb_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<ssh_lsc_credential id=\"%s\"/>"
                                        "<smb_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        ssh_credential,
                                        smb_credential);
      else if (ssh_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<ssh_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        ssh_credential);
      else if (smb_credential)
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "<ssh_lsc_credential id=\"%s\"/>"
                                        "</create_target>",
                                        name,
                                        hosts,
                                        smb_credential);
      else
        ret = openvas_server_sendf_xml (session,
                                        "<create_target>"
                                        "<name>%s</name>"
                                        "<hosts>%s</hosts>"
                                        "</create_target>",
                                        name,
                                        hosts);
    }

  if (ret) return -1;

  ret = omp_read_create_response (session, uuid);
  if (ret == 201)
    return 0;
  return ret;
}

/**
 * @brief Delete a target.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   id          UUID of target.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_delete_target (gnutls_session_t* session,
                   const char* id)
{
  if (openvas_server_sendf (session,
                            "<delete_target target_id=\"%s\"/>",
                            id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Create a config, given the config description as a string.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Config name.
 * @param[in]   comment     Config comment.
 * @param[in]   config      Config configuration.
 * @param[in]   config_len  Length of config.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_create_config (gnutls_session_t* session,
                   const char* name,
                   const char* comment,
                   const char* config,
                   unsigned int config_len)
{
  /* Convert the file contents to base64. */

  gchar* new_config_file = strlen (config)
                           ? g_base64_encode ((guchar*) config, config_len)
                           : g_strdup ("");

  /* Create the OMP request. */

  gchar* new_config_request;
  if (comment)
    new_config_request = g_markup_printf_escaped ("<create_config>"
                                                  "<name>%s</name>"
                                                  "<comment>%s</comment>"
                                                  "<rcfile>%s</rcfile>"
                                                  "</create_config>",
                                                  name,
                                                  comment,
                                                  new_config_file);
  else
    new_config_request = g_markup_printf_escaped ("<create_config>"
                                                  "<name>%s</name>"
                                                  "<rcfile>%s</rcfile>"
                                                  "</create_config>",
                                                  name,
                                                  new_config_file);
  g_free (new_config_file);

  /* Send the request. */

  int ret = openvas_server_send (session, new_config_request);
  g_free (new_config_request);
  if (ret) return -1;

  return check_response (session);
}

/**
 * @brief Create a config, given the config description as an RC file.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Config name.
 * @param[in]   comment     Config comment.
 * @param[in]   file_name   Name of RC file.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_create_config_from_rc_file (gnutls_session_t* session,
                                const char* name,
                                const char* comment,
                                const char* file_name)
{
  gchar* new_config_rc = NULL;
  gsize new_config_rc_len;
  GError* error = NULL;
  int ret;

  /* Read in the RC file. */

  g_file_get_contents (file_name,
                       &new_config_rc,
                       &new_config_rc_len,
                       &error);
  if (error)
    {
      g_error_free (error);
      return -1;
    }

  ret = omp_create_config (session,
                           name,
                           comment,
                           new_config_rc,
                           new_config_rc_len);
  g_free (new_config_rc);
  return ret;
}

/**
 * @brief Delete a config.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   id          UUID of config.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_delete_config (gnutls_session_t* session,
                   const char* id)
{
  if (openvas_server_sendf (session,
                            "<delete_config config_id=\"%s\"/>",
                            id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Create an LSC Credential.
 *
 * @param[in]   session   Pointer to GNUTLS session.
 * @param[in]   name      Name of LSC Credential.
 * @param[in]   login     Login associated with name.
 * @param[in]   password  Password, or NULL for autogenerated credentials.
 * @param[in]   comment   LSC Credential comment.
 * @param[out]  uuid      Either NULL or address for UUID of created credential.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_create_lsc_credential (gnutls_session_t* session,
                           const char* name,
                           const char* login,
                           const char* password,
                           const char* comment,
                           char** uuid)
{
  int ret;

  /* Create the OMP request. */

  gchar* new_task_request;
  if (password)
    {
      if (comment)
        new_task_request = g_markup_printf_escaped ("<create_lsc_credential>"
                                                    "<name>%s</name>"
                                                    "<login>%s</login>"
                                                    "<password>%s</password>"
                                                    "<comment>%s</comment>"
                                                    "</create_lsc_credential>",
                                                    name,
                                                    login,
                                                    password,
                                                    comment);
      else
        new_task_request = g_markup_printf_escaped ("<create_lsc_credential>"
                                                    "<name>%s</name>"
                                                    "<login>%s</login>"
                                                    "<password>%s</password>"
                                                    "</create_lsc_credential>",
                                                    name,
                                                    login,
                                                    password);
    }
  else
    {
      if (comment)
        new_task_request = g_markup_printf_escaped ("<create_lsc_credential>"
                                                    "<name>%s</name>"
                                                    "<login>%s</login>"
                                                    "<comment>%s</comment>"
                                                    "</create_lsc_credential>",
                                                    name,
                                                    login,
                                                    comment);
      else
        new_task_request = g_markup_printf_escaped ("<create_lsc_credential>"
                                                    "<name>%s</name>"
                                                    "<login>%s</login>"
                                                    "</create_lsc_credential>",
                                                    name,
                                                    login);
    }

  /* Send the request. */

  ret = openvas_server_send (session, new_task_request);
  g_free (new_task_request);
  if (ret) return -1;

  ret = omp_read_create_response (session, uuid);
  if (ret == 201)
    return 0;
  return ret;
}

/**
 * @brief Create an LSC Credential with a key.
 *
 * @param[in]   session      Pointer to GNUTLS session.
 * @param[in]   name         Name of LSC Credential.
 * @param[in]   login        Login associated with name.
 * @param[in]   passphrase   Passphrase for public key.
 * @param[in]   public_key   Public key.
 * @param[in]   private_key  Private key.
 * @param[in]   comment      LSC Credential comment.
 * @param[out]  uuid         Either NULL or address for UUID of created
 *                           credential.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_create_lsc_credential_key (gnutls_session_t *session,
                               const char *name,
                               const char *login,
                               const char *passphrase,
                               const char *public_key,
                               const char *private_key,
                               const char *comment,
                               char **uuid)
{
  int ret;

  /* Create the OMP request. */

  gchar* request;
  if (comment)
    request = g_markup_printf_escaped ("<create_lsc_credential>"
                                       "<name>%s</name>"
                                       "<login>%s</login>"
                                       "<key>"
                                       "<phrase>%s</phrase>"
                                       "<public>%s</public>"
                                       "<private>%s</private>"
                                       "</key>"
                                       "<comment>%s</comment>"
                                       "</create_lsc_credential>",
                                       name,
                                       login,
                                       passphrase
                                        ? passphrase
                                        : "",
                                       public_key,
                                       private_key,
                                       comment);
  else
    request = g_markup_printf_escaped ("<create_lsc_credential>"
                                       "<name>%s</name>"
                                       "<login>%s</login>"
                                       "<key>"
                                       "<phrase>%s</phrase>"
                                       "<public>%s</public>"
                                       "<private>%s</private>"
                                       "</key>"
                                       "</create_lsc_credential>",
                                       name,
                                       login,
                                       passphrase
                                        ? passphrase
                                        : "",
                                       public_key,
                                       private_key);

  /* Send the request. */

  ret = openvas_server_send (session, request);
  g_free (request);
  if (ret) return -1;

  ret = omp_read_create_response (session, uuid);
  if (ret == 201)
    return 0;
  return ret;
}

/**
 * @brief Delete a LSC credential.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   uuid        UUID of LSC credential.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_delete_lsc_credential (gnutls_session_t* session,
                           const char* id)
{
  if (openvas_server_sendf (session,
                            "<delete_lsc_credential lsc_credential_id=\"%s\"/>",
                            id)
      == -1)
    return -1;

  return check_response (session);
}

/**
 * @brief Create an agent.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Name of agent.
 * @param[in]   comment     Agent comment.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_create_agent (gnutls_session_t* session,
                  const char* name,
                  const char* comment)
{
  int ret;

  /* Create the OMP request. */

  gchar* new_task_request;
  if (comment)
    new_task_request = g_markup_printf_escaped ("<create_agent>"
                                                "<name>%s</name>"
                                                "<comment>%s</comment>"
                                                "</create_agent>",
                                                name,
                                                comment);
  else
    new_task_request = g_markup_printf_escaped ("<create_agent>"
                                                "<name>%s</name>"
                                                "</create_agent>",
                                                name);

  /* Send the request. */

  ret = openvas_server_send (session, new_task_request);
  g_free (new_task_request);
  if (ret) return -1;

  return check_response (session);
}

/**
 * @brief Delete an agent.
 *
 * @param[in]   session     Pointer to GNUTLS session.
 * @param[in]   name        Name of agent.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_delete_agent (gnutls_session_t* session,
                  const char* name)
{
  int ret;

  /* Create the OMP request. */

  gchar* new_task_request;
  new_task_request = g_markup_printf_escaped ("<delete_agent>"
                                              "<name>%s</name>"
                                              "</delete_agent>",
                                              name);

  /* Send the request. */

  ret = openvas_server_send (session, new_task_request);
  g_free (new_task_request);
  if (ret) return -1;

  return check_response (session);
}

/**
 * @brief Get NVT Information.
 *
 * @param[in]  session         Pointer to GNUTLS session.
 * @param[in]  oid             OID of NVT or NULL for all NVTs.
 * @param[out] response        Status return. On success contains GET_TASKS
 *                             response.
 *
 * @return 0 on success, -1 on error.
 */
int
omp_get_nvt_details_503 (gnutls_session_t* session, const char * oid,
                         entity_t* response)
{
  gchar* request;
  int ret;

  if (oid)
    request = g_markup_printf_escaped ("<get_nvts"
                                       " nvt_oid=\"%s\""
                                       " details=\"1\""
                                       " preferences=\"1\"/>",
                                       oid);
  else
    request = g_strdup ("<get_nvts"
                        " details=\"1\""
                        " preference_count=\"1\"/>");

  ret = get_omp_response_503 (session, request, response);

  g_free (request);

  return ret;
}

/**
 * @brief Get system reports.
 *
 * @param[in]  session  Pointer to GNUTLS session.
 * @param[in]  name     Name of system report.  NULL for all.
 * @param[in]  brief    Whether to request brief response.
 * @param[out] reports  Reports return.  On success contains GET_SYSTEM_REPORTS
 *                      response.
 *
 * @return 0 on success, -1 or OMP response code on error.
 */
int
omp_get_system_reports (gnutls_session_t* session, const char* name, int brief,
                        entity_t *reports)
{
  int ret;
  const char *status_code;

  if (name)
    {
      if (openvas_server_sendf (session,
                                "<get_system_reports name=\"%s\" brief=\"%i\"/>",
                                name,
                                brief)
          == -1)
        return -1;
    }
  else if (openvas_server_sendf (session,
                                 "<get_system_reports brief=\"%i\"/>",
                                 brief)
           == -1)
    return -1;

  /* Read the response. */

  *reports = NULL;
  if (read_entity (session, reports)) return -1;

  /* Check the response. */

  status_code = entity_attribute (*reports, "status");
  if (status_code == NULL)
    {
      free_entity (*reports);
      return -1;
    }
  if (strlen (status_code) == 0)
    {
      free_entity (*reports);
      return -1;
    }
  if (status_code[0] == '2') return 0;
  ret = (int) strtol (status_code, NULL, 10);
  free_entity (*reports);
  if (errno == ERANGE) return -1;
  return ret;
}
