#ifndef _AUTH_H_
#define _AUTH_H_

void handle_login(int client_fd, const char *data, int *user_id);

void handle_register(int client_fd, const char *data, int *user_id);

#endif