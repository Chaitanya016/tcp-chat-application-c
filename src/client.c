#include "../include/common.h"

typedef struct
{
    int socket_fd;
    char username[MAX_NAME];
} client_session_t;

static void trim_newline(char *text);
static int read_line(char *buffer, size_t buffer_size);
static int create_client_socket(void);
static int authenticate_user(int socket_fd, char *username_out);
static int send_auth_request(int socket_fd, const auth_info *request);
static int receive_auth_response(int socket_fd);
static void run_chat_loop(client_session_t *session);
static void send_private_message(client_session_t *session);
static void send_group_message(client_session_t *session);
static void send_exit_message(client_session_t *session);
static void *receive_messages(void *arg);

int main(void)
{
    int socket_fd;
    client_session_t session;
    pthread_t receiver_thread;

    socket_fd = create_client_socket();
    if (socket_fd < 0)
    {
        return EXIT_FAILURE;
    }

    memset(&session, 0, sizeof(session));
    session.socket_fd = socket_fd;

    if (!authenticate_user(socket_fd, session.username))
    {
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if (pthread_create(&receiver_thread, NULL, receive_messages, &session) != 0)
    {
        perror("pthread_create");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    run_chat_loop(&session);

    pthread_join(receiver_thread, NULL);
    return EXIT_SUCCESS;
}

static int create_client_socket(void)
{
    int socket_fd;
    struct sockaddr_in server_addr;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(COMM_PORT);
    server_addr.sin_addr.s_addr = inet_addr(HOST_ADDR);

    if (connect(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(socket_fd);
        return -1;
    }

    printf("Connected to server at %s:%d\n", HOST_ADDR, COMM_PORT);
    return socket_fd;
}

static int authenticate_user(int socket_fd, char *username_out)
{
    auth_info request;
    char input_buffer[32];

    memset(&request, 0, sizeof(request));

    printf("1. Login\n");
    printf("2. Register\n");
    printf("Choose an option: ");
    if (!read_line(input_buffer, sizeof(input_buffer)))
    {
        return 0;
    }

    request.action = atoi(input_buffer);
    if (request.action != 1 && request.action != 2)
    {
        printf("Invalid choice.\n");
        return 0;
    }

    printf("Username: ");
    if (!read_line(request.uname, sizeof(request.uname)))
    {
        return 0;
    }

    printf("Password: ");
    if (!read_line(request.passwd, sizeof(request.passwd)))
    {
        return 0;
    }

    if (!send_auth_request(socket_fd, &request))
    {
        return 0;
    }

    if (!receive_auth_response(socket_fd))
    {
        return 0;
    }

    strncpy(username_out, request.uname, MAX_NAME - 1);
    username_out[MAX_NAME - 1] = '\0';
    return 1;
}

static int send_auth_request(int socket_fd, const auth_info *request)
{
    if (request == NULL)
    {
        return 0;
    }

    if (send(socket_fd, request, sizeof(*request), 0) < 0)
    {
        perror("send");
        return 0;
    }

    return 1;
}

static int receive_auth_response(int socket_fd)
{
    msg_frame response;
    ssize_t bytes_received;

    memset(&response, 0, sizeof(response));
    bytes_received = recv(socket_fd, &response, sizeof(response), 0);
    if (bytes_received <= 0)
    {
        printf("Server closed the connection during authentication.\n");
        return 0;
    }

    if (strncmp(response.data, "OK", sizeof(response.data)) == 0)
    {
        printf("Authentication successful.\n");
        return 1;
    }

    printf("Authentication failed: %s\n", response.data);
    return 0;
}

static void run_chat_loop(client_session_t *session)
{
    char input_buffer[32];

    if (session == NULL)
    {
        return;
    }

    while (1)
    {
        printf("\n1. Private Chat\n");
        printf("2. Group Chat\n");
        printf("3. Exit\n");
        printf("Choose an option: ");

        if (!read_line(input_buffer, sizeof(input_buffer)))
        {
            send_exit_message(session);
            shutdown(session->socket_fd, SHUT_RDWR);
            close(session->socket_fd);
            return;
        }

        if (strcmp(input_buffer, "1") == 0)
        {
            send_private_message(session);
        }
        else if (strcmp(input_buffer, "2") == 0)
        {
            send_group_message(session);
        }
        else if (strcmp(input_buffer, "3") == 0)
        {
            send_exit_message(session);
            shutdown(session->socket_fd, SHUT_RDWR);
            close(session->socket_fd);
            return;
        }
        else
        {
            printf("Invalid choice.\n");
        }
    }
}

static void send_private_message(client_session_t *session)
{
    msg_frame message;

    if (session == NULL)
    {
        return;
    }

    memset(&message, 0, sizeof(message));
    message.mtype = PRIVATE_CHAT;
    strncpy(message.from, session->username, sizeof(message.from) - 1);
    message.from[sizeof(message.from) - 1] = '\0';

    printf("Send to: ");
    if (!read_line(message.to, sizeof(message.to)))
    {
        return;
    }

    printf("Message: ");
    if (!read_line(message.data, sizeof(message.data)))
    {
        return;
    }

    if (send(session->socket_fd, &message, sizeof(message), 0) < 0)
    {
        perror("send");
    }
}

static void send_group_message(client_session_t *session)
{
    msg_frame message;

    if (session == NULL)
    {
        return;
    }

    memset(&message, 0, sizeof(message));
    message.mtype = GROUP_CHAT;
    strncpy(message.from, session->username, sizeof(message.from) - 1);
    message.from[sizeof(message.from) - 1] = '\0';
    strncpy(message.to, "ALL", sizeof(message.to) - 1);
    message.to[sizeof(message.to) - 1] = '\0';

    printf("Message: ");
    if (!read_line(message.data, sizeof(message.data)))
    {
        return;
    }

    if (send(session->socket_fd, &message, sizeof(message), 0) < 0)
    {
        perror("send");
    }
}

static void send_exit_message(client_session_t *session)
{
    msg_frame message;

    if (session == NULL)
    {
        return;
    }

    memset(&message, 0, sizeof(message));
    message.mtype = EXIT_MSG;
    strncpy(message.from, session->username, sizeof(message.from) - 1);
    message.from[sizeof(message.from) - 1] = '\0';

    if (send(session->socket_fd, &message, sizeof(message), 0) < 0)
    {
        perror("send");
    }
}

static void *receive_messages(void *arg)
{
    client_session_t *session;
    msg_frame message;
    ssize_t bytes_received;

    session = (client_session_t *)arg;
    if (session == NULL)
    {
        return NULL;
    }

    while (1)
    {
        memset(&message, 0, sizeof(message));
        bytes_received = recv(session->socket_fd, &message, sizeof(message), 0);
        if (bytes_received <= 0)
        {
            printf("\nDisconnected from server.\n");
            break;
        }

        if (message.mtype == USR_LIST)
        {
            printf("\n[Online] %s\n", message.from);
        }
        else if (message.mtype == PRIVATE_CHAT)
        {
            printf("\n[Private] %s: %s\n", message.from, message.data);
        }
        else if (message.mtype == GROUP_CHAT)
        {
            printf("\n[Group] %s: %s\n", message.from, message.data);
        }
        else if (message.mtype == NOTIFY)
        {
            printf("\n[Notice] %s\n", message.data);
        }
        else if (message.mtype == NOT_AVAIL)
        {
            printf("\n[Notice] %s\n", message.data);
        }
    }

    return NULL;
}

static int read_line(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return 0;
    }

    if (fgets(buffer, (int)buffer_size, stdin) == NULL)
    {
        return 0;
    }

    trim_newline(buffer);
    return 1;
}

static void trim_newline(char *text)
{
    size_t length;

    if (text == NULL)
    {
        return;
    }

    length = strlen(text);
    if (length > 0 && text[length - 1] == '\n')
    {
        text[length - 1] = '\0';
    }
}
