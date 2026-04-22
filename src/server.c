#include "../include/common.h"

#define SERVER_BACKLOG 10
#define USERS_FILE "users.txt"

typedef struct
{
    int client_fd;
    struct sockaddr_in client_addr;
} client_context_t;

typedef struct user_node
{
    char username[MAX_NAME];
    char password[MAX_PASS];
    struct user_node *next;
} user_node_t;

typedef struct active_user_node
{
    char username[MAX_NAME];
    int socket_fd;
    struct active_user_node *next;
} active_user_node_t;

static user_node_t *user_list_head = NULL;
static active_user_node_t *active_user_list_head = NULL;
static pthread_mutex_t active_user_mutex = PTHREAD_MUTEX_INITIALIZER;

void load_users(void);
void save_users(void);
void prepend_user(const char *username, const char *password);
int find_username(const char *username);
int verify_password(const char *username, const char *password);
void add_active_user(const char *uname, int fd);
void remove_active_user(const char *uname);
int find_active_fd(const char *uname);
void send_online_list(int conn_fd, const char *self_name);
void broadcast_login(const char *uname);
void broadcast_logout(const char *uname);
static void log_info(const char *message);
static void log_error(const char *message);
static void log_client_connected(const struct sockaddr_in *client_addr);
static void *handle_client(void *arg);
static void copy_text(char *destination, size_t destination_size, const char *source);
static void build_notification_frame(msg_frame *frame, const char *username, const char *status_text);
static int send_frame_to_client(int conn_fd, const msg_frame *frame);
static void send_auth_response(int conn_fd, const char *message_text);
static void send_not_available_message(int conn_fd, const char *receiver_name);
static void broadcast_group_message(const msg_frame *message);

int main(void)
{
    int server_fd;
    struct sockaddr_in server_addr;

    load_users();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        log_error("socket creation failed");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(COMM_PORT);
    server_addr.sin_addr.s_addr = inet_addr(HOST_ADDR);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        log_error("bind failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, SERVER_BACKLOG) < 0)
    {
        log_error("listen failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    log_info("server started successfully");
    printf("Listening on %s:%d\n", HOST_ADDR, COMM_PORT);

    while (1)
    {
        client_context_t *client_ctx;
        pthread_t client_thread;
        socklen_t client_len;

        client_ctx = malloc(sizeof(*client_ctx));
        if (client_ctx == NULL)
        {
            log_error("memory allocation failed for client context");
            continue;
        }

        client_len = sizeof(client_ctx->client_addr);
        client_ctx->client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_ctx->client_addr,
            &client_len);

        if (client_ctx->client_fd < 0)
        {
            log_error("accept failed");
            free(client_ctx);
            continue;
        }

        log_client_connected(&client_ctx->client_addr);

        if (pthread_create(&client_thread, NULL, handle_client, client_ctx) != 0)
        {
            log_error("pthread_create failed");
            close(client_ctx->client_fd);
            free(client_ctx);
            continue;
        }

        pthread_detach(client_thread);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}

static void *handle_client(void *arg)
{
    client_context_t *client_ctx;
    auth_info credentials;
    msg_frame message;
    char current_username[MAX_NAME];
    int is_active_user;
    ssize_t bytes_received;

    client_ctx = (client_context_t *)arg;

    if (client_ctx == NULL)
    {
        return NULL;
    }

    current_username[0] = '\0';
    is_active_user = 0;

    log_info("client handler thread started");

    memset(&credentials, 0, sizeof(credentials));
    bytes_received = recv(client_ctx->client_fd, &credentials, sizeof(credentials), 0);
    if (bytes_received <= 0)
    {
        close(client_ctx->client_fd);
        free(client_ctx);
        return NULL;
    }

    if (credentials.action == 1)
    {
        if (!find_username(credentials.uname))
        {
            send_auth_response(client_ctx->client_fd, "USERNAME NOT FOUND");
            close(client_ctx->client_fd);
            free(client_ctx);
            return NULL;
        }

        if (!verify_password(credentials.uname, credentials.passwd))
        {
            send_auth_response(client_ctx->client_fd, "INVALID PASSWORD");
            close(client_ctx->client_fd);
            free(client_ctx);
            return NULL;
        }

        if (find_active_fd(credentials.uname) >= 0)
        {
            send_auth_response(client_ctx->client_fd, "USER ALREADY ONLINE");
            close(client_ctx->client_fd);
            free(client_ctx);
            return NULL;
        }

        send_auth_response(client_ctx->client_fd, "OK");
    }
    else if (credentials.action == 2)
    {
        if (find_username(credentials.uname))
        {
            send_auth_response(client_ctx->client_fd, "USERNAME TAKEN");
            close(client_ctx->client_fd);
            free(client_ctx);
            return NULL;
        }

        prepend_user(credentials.uname, credentials.passwd);
        save_users();
        send_auth_response(client_ctx->client_fd, "OK");
    }
    else
    {
        send_auth_response(client_ctx->client_fd, "INVALID ACTION");
        close(client_ctx->client_fd);
        free(client_ctx);
        return NULL;
    }

    copy_text(current_username, sizeof(current_username), credentials.uname);
    add_active_user(current_username, client_ctx->client_fd);
    is_active_user = 1;

    send_online_list(client_ctx->client_fd, current_username);
    broadcast_login(current_username);

    while (1)
    {
        memset(&message, 0, sizeof(message));
        bytes_received = recv(client_ctx->client_fd, &message, sizeof(message), 0);

        if (bytes_received <= 0)
        {
            break;
        }

        if (message.mtype == PRIVATE_CHAT)
        {
            int receiver_fd;

            receiver_fd = find_active_fd(message.to);
            if (receiver_fd >= 0)
            {
                send_frame_to_client(receiver_fd, &message);
            }
            else
            {
                send_not_available_message(client_ctx->client_fd, message.to);
            }
        }
        else if (message.mtype == GROUP_CHAT)
        {
            broadcast_group_message(&message);
        }
        else if (message.mtype == EXIT_MSG)
        {
            remove_active_user(current_username);
            broadcast_logout(current_username);
            is_active_user = 0;
            close(client_ctx->client_fd);
            free(client_ctx);
            return NULL;
        }
    }

    if (is_active_user)
    {
        remove_active_user(current_username);
        broadcast_logout(current_username);
    }

    close(client_ctx->client_fd);
    free(client_ctx);
    return NULL;
}

void load_users(void)
{
    FILE *users_file;
    char line[MAX_NAME + MAX_PASS + 4];

    users_file = fopen(USERS_FILE, "r");
    if (users_file == NULL)
    {
        log_info("users file not found, starting with empty user list");
        return;
    }

    while (fgets(line, sizeof(line), users_file) != NULL)
    {
        char *separator;
        char *newline_pos;
        char username[MAX_NAME];
        char password[MAX_PASS];
        size_t username_len;
        size_t password_len;

        newline_pos = strchr(line, '\n');
        if (newline_pos != NULL)
        {
            *newline_pos = '\0';
        }

        separator = strchr(line, ';');
        if (separator == NULL)
        {
            continue;
        }

        *separator = '\0';
        separator++;

        if (line[0] == '\0' || separator[0] == '\0')
        {
            continue;
        }

        username_len = strnlen(line, MAX_NAME);
        password_len = strnlen(separator, MAX_PASS);

        if (username_len >= MAX_NAME || password_len >= MAX_PASS)
        {
            continue;
        }

        strncpy(username, line, sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';

        strncpy(password, separator, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';

        prepend_user(username, password);
    }

    fclose(users_file);
    log_info("users loaded from file");
}

void save_users(void)
{
    FILE *users_file;
    user_node_t *current_user;

    users_file = fopen(USERS_FILE, "w");
    if (users_file == NULL)
    {
        log_error("failed to open users file for writing");
        return;
    }

    current_user = user_list_head;
    while (current_user != NULL)
    {
        fprintf(
            users_file,
            "%s;%s\n",
            current_user->username,
            current_user->password);
        current_user = current_user->next;
    }

    fclose(users_file);
    log_info("users saved to file");
}

void prepend_user(const char *username, const char *password)
{
    user_node_t *new_user;

    if (username == NULL || password == NULL)
    {
        return;
    }

    new_user = malloc(sizeof(*new_user));
    if (new_user == NULL)
    {
        log_error("memory allocation failed for user node");
        return;
    }

    strncpy(new_user->username, username, sizeof(new_user->username) - 1);
    new_user->username[sizeof(new_user->username) - 1] = '\0';

    strncpy(new_user->password, password, sizeof(new_user->password) - 1);
    new_user->password[sizeof(new_user->password) - 1] = '\0';

    new_user->next = user_list_head;
    user_list_head = new_user;
}

int find_username(const char *username)
{
    user_node_t *current_user;

    if (username == NULL)
    {
        return 0;
    }

    current_user = user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, username, MAX_NAME) == 0)
        {
            return 1;
        }

        current_user = current_user->next;
    }

    return 0;
}

int verify_password(const char *username, const char *password)
{
    user_node_t *current_user;

    if (username == NULL || password == NULL)
    {
        return 0;
    }

    current_user = user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, username, MAX_NAME) == 0 &&
            strncmp(current_user->password, password, MAX_PASS) == 0)
        {
            return 1;
        }

        current_user = current_user->next;
    }

    return 0;
}

void add_active_user(const char *uname, int fd)
{
    active_user_node_t *current_user;
    active_user_node_t *new_user;

    if (uname == NULL)
    {
        return;
    }

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, uname, MAX_NAME) == 0)
        {
            current_user->socket_fd = fd;
            pthread_mutex_unlock(&active_user_mutex);
            return;
        }

        current_user = current_user->next;
    }

    new_user = malloc(sizeof(*new_user));
    if (new_user == NULL)
    {
        pthread_mutex_unlock(&active_user_mutex);
        log_error("memory allocation failed for active user node");
        return;
    }

    copy_text(new_user->username, sizeof(new_user->username), uname);
    new_user->socket_fd = fd;
    new_user->next = active_user_list_head;
    active_user_list_head = new_user;

    pthread_mutex_unlock(&active_user_mutex);
}

void remove_active_user(const char *uname)
{
    active_user_node_t *current_user;
    active_user_node_t *previous_user;

    if (uname == NULL)
    {
        return;
    }

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    previous_user = NULL;

    while (current_user != NULL)
    {
        if (strncmp(current_user->username, uname, MAX_NAME) == 0)
        {
            if (previous_user == NULL)
            {
                active_user_list_head = current_user->next;
            }
            else
            {
                previous_user->next = current_user->next;
            }

            free(current_user);
            pthread_mutex_unlock(&active_user_mutex);
            return;
        }

        previous_user = current_user;
        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
}

int find_active_fd(const char *uname)
{
    active_user_node_t *current_user;
    int result_fd;

    if (uname == NULL)
    {
        return -1;
    }

    pthread_mutex_lock(&active_user_mutex);

    result_fd = -1;
    current_user = active_user_list_head;

    while (current_user != NULL)
    {
        if (strncmp(current_user->username, uname, MAX_NAME) == 0)
        {
            result_fd = current_user->socket_fd;
            break;
        }

        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
    return result_fd;
}

void send_online_list(int conn_fd, const char *self_name)
{
    active_user_node_t *current_user;

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    while (current_user != NULL)
    {
        if (self_name == NULL ||
            strncmp(current_user->username, self_name, MAX_NAME) != 0)
        {
            msg_frame frame;

            memset(&frame, 0, sizeof(frame));
            frame.mtype = USR_LIST;
            copy_text(frame.from, sizeof(frame.from), current_user->username);
            copy_text(frame.to, sizeof(frame.to), self_name == NULL ? "" : self_name);

            if (send(conn_fd, &frame, sizeof(frame), 0) < 0)
            {
                log_error("failed to send online user list");
                break;
            }
        }

        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
}

void broadcast_login(const char *uname)
{
    active_user_node_t *current_user;
    msg_frame frame;

    if (uname == NULL)
    {
        return;
    }

    build_notification_frame(&frame, uname, "joined");

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, uname, MAX_NAME) != 0)
        {
            if (send(current_user->socket_fd, &frame, sizeof(frame), 0) < 0)
            {
                log_error("failed to broadcast login notification");
            }
        }

        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
}

void broadcast_logout(const char *uname)
{
    active_user_node_t *current_user;
    msg_frame frame;

    if (uname == NULL)
    {
        return;
    }

    build_notification_frame(&frame, uname, "left");

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, uname, MAX_NAME) != 0)
        {
            if (send(current_user->socket_fd, &frame, sizeof(frame), 0) < 0)
            {
                log_error("failed to broadcast logout notification");
            }
        }

        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
}

static void log_info(const char *message)
{
    printf("[INFO] %s\n", message);
}

static void log_error(const char *message)
{
    perror(message);
}

static void log_client_connected(const struct sockaddr_in *client_addr)
{
    char client_ip[INET_ADDRSTRLEN];

    if (client_addr == NULL)
    {
        return;
    }

    if (inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip)) == NULL)
    {
        strncpy(client_ip, "unknown", sizeof(client_ip) - 1);
        client_ip[sizeof(client_ip) - 1] = '\0';
    }

    printf(
        "[INFO] client connected from %s:%d\n",
        client_ip,
        ntohs(client_addr->sin_port));
}

static void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0)
    {
        return;
    }

    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, destination_size - 1);
    destination[destination_size - 1] = '\0';
}

static void build_notification_frame(msg_frame *frame, const char *username, const char *status_text)
{
    if (frame == NULL)
    {
        return;
    }

    memset(frame, 0, sizeof(*frame));
    frame->mtype = NOTIFY;
    copy_text(frame->from, sizeof(frame->from), username);
    snprintf(frame->data, sizeof(frame->data), "%s %s", username, status_text);
}

static int send_frame_to_client(int conn_fd, const msg_frame *frame)
{
    if (frame == NULL)
    {
        return -1;
    }

    if (send(conn_fd, frame, sizeof(*frame), 0) < 0)
    {
        log_error("send failed");
        return -1;
    }

    return 0;
}

static void send_auth_response(int conn_fd, const char *message_text)
{
    msg_frame response;

    memset(&response, 0, sizeof(response));
    response.mtype = NOTIFY;
    copy_text(response.data, sizeof(response.data), message_text);
    send_frame_to_client(conn_fd, &response);
}

static void send_not_available_message(int conn_fd, const char *receiver_name)
{
    msg_frame response;

    memset(&response, 0, sizeof(response));
    response.mtype = NOT_AVAIL;
    copy_text(response.to, sizeof(response.to), receiver_name);
    snprintf(response.data, sizeof(response.data), "%s is not online", receiver_name);
    send_frame_to_client(conn_fd, &response);
}

static void broadcast_group_message(const msg_frame *message)
{
    active_user_node_t *current_user;

    if (message == NULL)
    {
        return;
    }

    pthread_mutex_lock(&active_user_mutex);

    current_user = active_user_list_head;
    while (current_user != NULL)
    {
        if (strncmp(current_user->username, message->from, MAX_NAME) != 0)
        {
            if (send(current_user->socket_fd, message, sizeof(*message), 0) < 0)
            {
                log_error("failed to broadcast group message");
            }
        }

        current_user = current_user->next;
    }

    pthread_mutex_unlock(&active_user_mutex);
}
