#include <sys/types.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "hash.h"
#include "ftree.h"

struct client {
	int fd;
	struct in_addr ipaddr;
	int state;
	struct request *req;
	struct client *next;
};

static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
int bindandlisten(void);
int check_same(struct request *request, int lst, struct stat *buf);
int copy_file(char *source, char *basename_relative_path, int *sock_fd, struct sockaddr_in *server);

/* Connect to the server with host and port. Create a new socket
 * file descriptor to connect to and communicate with the server.
 * Copy the files at source in client to the server.
 * Return 0 if no errors are encountered during file transfers, or -1 otherwise.
 */
int rcopy_client(char *source, char *host, unsigned short port) {
    // Create the socket FD
    int *sock_fd = malloc(sizeof(int));
    *sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct hostent *he;
    if (*sock_fd < 0) {
        perror("client: socket");
        return 1;
    }
    
    // Get host info
    if ((he = gethostbyname(host)) == NULL) {
        perror("gethostbyname");
        exit(1);
    }

    // Set the IP and port of the server to connect to
    struct sockaddr_in *server = malloc(sizeof(struct sockaddr_in));
    server->sin_family = AF_INET;
    server->sin_port = htons(port);
    server->sin_addr = *((struct in_addr *)he->h_addr);

    // Connect to the server
    if (connect(*sock_fd, (struct sockaddr *)server, sizeof(*server)) == -1) {
        perror("connect");
        close(*sock_fd);
        return 1;
    }
    
    char *bname = basename(source);
    
    // Copy files
    int error = copy_file(source, bname, sock_fd, server);
    
    // Close the socket after all files have been transferred
    close(*sock_fd);
    
    // Free the memory allocated by the client
    free(server);
    free(sock_fd);
    
    // Return 0 only if copy_file returned 0,
    // having encountered no errors during file transfers
    // Otherwise, return -1
    return error;
}

/* Copy the file at source in client to basename_relative_path in server using the socket sock_fd.
 * Fork a new client and create a new socket when the server responds to a request with SENDFILE,
 * and transmit the data from the file to the server. If source is a directory,
 * walk through the files rooted there and copy files accordingly.
 * Return 0 if no errors are encountered during file transfers, or -1 otherwise.
 */
int copy_file(char *source, char *basename_relative_path, int *sock_fd, struct sockaddr_in *server) {
    int error = 0;
    
    // Get file status
    struct stat sourcebuf;
    if (lstat(source, &sourcebuf) == -1) {
        fprintf(stderr, "Error encountered while copying %s: lstat\n", basename_relative_path);
        return 1;
    }
    
    char *bname = basename(source);
    
    // Initialize and fill in the struct request to send to the server and
    // translate any numeric types to network order
    struct request *req = malloc(sizeof(struct request));
    strncpy(req->path, basename_relative_path, MAXPATH);
    req->mode = sourcebuf.st_mode;
    req->size = sourcebuf.st_size;
    int mode = htonl(req->mode);
    int size = htonl(req->size);
    
    // If source is a file
    if (S_ISREG(sourcebuf.st_mode)) {
        // If the file is a regular file and not a link
        if (bname[0] != '.') {
            // Open the file for reading
            FILE *fsource = fopen(source, "r");
            if (fsource == NULL) {
                fprintf(stderr, "Error encountered while copying %s: fopen\n", basename_relative_path);
                return 1;
            }
            
            // Fill in the request type and hash to send to the server
            char *hash_val = malloc(BLOCKSIZE);
            strncpy(req->hash, hash(hash_val, fsource), BLOCKSIZE);
            req->type = REGFILE;
            
            // Send the request to the server
            if (write(*sock_fd, &(req->type), sizeof(int)) != sizeof(int) ||
                write(*sock_fd, req->path, sizeof(req->path)) != MAXPATH ||
                write(*sock_fd, &(mode), sizeof(int)) != sizeof(int) ||
                write(*sock_fd, req->hash, sizeof(req->hash)) != BLOCKSIZE ||
                write(*sock_fd, &(size), sizeof(int)) != sizeof(int)) {
                fprintf(stderr, "Error encountered while copying %s: write\n", basename_relative_path);
                return 1;
            }
            
            // Wait for the server's response
            // First, we prepare to listen to multiple
            // file descriptors by initializing a set of file descriptors
            int max_fd = *sock_fd;
            fd_set all_fds, listen_fds;
            FD_ZERO(&all_fds);
            FD_SET(*sock_fd, &all_fds);
            int response = -1;
            
            while (response == -1) {
                // select updates the fd_set it receives, so we always use a copy and retain the original.
                listen_fds = all_fds;
                int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
                if (nready == -1) {
                    fprintf(stderr, "Error encountered while copying %s: client: select\n", basename_relative_path);
                    return 1;
                }
        
                // If the server is ready for reading ...
                if (FD_ISSET(*sock_fd, &listen_fds)) {
                    int num_read = read(*sock_fd, &response, sizeof(int));
                    if (num_read != sizeof(int)) {
                        printf("1");
                        fprintf(stderr, "Error encountered while copying %s: read\n", basename_relative_path);
                        return 1;
                    }
                }
            }
            
            if (response != sizeof(int)) {
                fprintf(stderr, "Error encountered while copying %s: read\n", basename_relative_path);
                return 1;
                printf("2");
            }
            
            // If the server asks for the file to be sent,
            // fork a new client process
            if (response == SENDFILE) {
                int result = fork();
                
                if (result == -1) {
                    fprintf(stderr, "Error encountered while copying %s: fork\n", basename_relative_path);
                    return 1;
                } else if (result == 0) {
                    // Child process
                    // Initiate a new connection with the server
                    // Create the socket FD.
                    int *fork_sock_fd = malloc(sizeof(int));
                    *fork_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (*fork_sock_fd < 0) {
                        fprintf(stderr, "Error encountered while copying %s: socket\n", basename_relative_path);
                        return 1;
                    }
    
                    if (connect(*fork_sock_fd, (struct sockaddr *)server, sizeof(*server)) == -1) {
                        fprintf(stderr, "Error encountered while copying %s: connect\n", basename_relative_path);
                        close(*fork_sock_fd);
                        return 1;
                    }
                    
                    // Initialize and fill in the struct request to send to the server and
                    // translate any numeric types to network order
                    struct request *req = malloc(sizeof(struct request));
                    strncpy(req->path, basename_relative_path, MAXPATH);
                    req->mode = sourcebuf.st_mode;
                    req->size = sourcebuf.st_size;
                    int mode = htonl(req->mode);
                    int size = htonl(req->size);
                
                    // Identify as a file sender client and
                    // send a new request with the type TRANSFILE
                    req->type = TRANSFILE;
                    if (write(*fork_sock_fd, &(req->type), sizeof(int)) != sizeof(int) ||
                        write(*fork_sock_fd, req->path, sizeof(req->path)) != MAXPATH ||
                        write(*fork_sock_fd, &(mode), sizeof(int)) != sizeof(int) ||
                        write(*fork_sock_fd, req->hash, sizeof(req->hash)) != BLOCKSIZE ||
                        write(*fork_sock_fd, &(size), sizeof(int)) != sizeof(int)) {
                        fprintf(stderr, "Error encountered while copying %s: write\n", basename_relative_path);
                        return 1;
                    }
                    
                    // Free the memory allocated by the request struct
                    free(hash_val);
                    free(req);
                    
                    // Transmit the data from the file to the server
                    char buf[MAXDATA];
                    if (fread(buf, 1, sizeof(buf), fsource) != MAXDATA) {
                        fprintf(stderr, "Error encountered while copying %s: read\n", basename_relative_path);
                        printf("3");
                        return 1;
                    }
                    
                    if (write(*fork_sock_fd, buf, sizeof(buf)) != MAXDATA) {
                        fprintf(stderr, "Error encountered while copying %s: write\n", basename_relative_path);
                        return 1;
                    }
                    
                    // Wait for the server's response
                    // First, we prepare to listen to multiple
                    // file descriptors by initializing a set of file descriptors.
                    int max_fd = *sock_fd;
                    fd_set all_fds, listen_fds;
                    FD_ZERO(&all_fds);
                    FD_SET(*sock_fd, &all_fds);
                    int message = -1;
    
                    while (message == -1) {
                        // select updates the fd_set it receives, so we always use a copy and retain the original.
                        listen_fds = all_fds;
                        int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
                        if (nready == -1) {
                            fprintf(stderr, "Error encountered while copying %s: client: select\n", basename_relative_path);
                        }
        
                        // If the server is ready for reading ...
                        if (FD_ISSET(*sock_fd, &listen_fds)) {
                            int num_read = read(*sock_fd, &message, sizeof(int));
                            if (num_read != sizeof(int)) {
                                printf("4");
                                fprintf(stderr, "Error encountered while copying %s: read\n", basename_relative_path);
                                return 1;
                            }
                        }
                    }
                    
                    // If the message is OK, close the socket and exit
                    // Otherwise, error
                    if (message == OK) {
                        close(*fork_sock_fd);
                        exit(0);
                    } else if (message == ERROR) {
                        close(*fork_sock_fd);
                        fprintf(stderr, "Error encountered while copying %s: server ERROR\n", basename_relative_path);
                        exit(1);
                    }
                } else if (result > 0) {
                    // Parent process
                    int status;
                    if (wait(&status) == -1) {
                        fprintf(stderr, "Error encountered while copying %s: wait\n", basename_relative_path);
                        return 1;
                    }
                }
            } else if (response == ERROR) {
                // If the files are incompatible
                fprintf(stderr, "Error encountered while copying %s: server ERROR\n", basename_relative_path);
                return 1;
            }
        }        
    } else if (S_ISDIR(sourcebuf.st_mode)) {
        // If the file is a directory
        // fill in the request type and hash to send to the server
        int index;
        char *hash_val = malloc(BLOCKSIZE);
        for (index = 0; index < BLOCKSIZE; index++) {
            hash_val[index] = '\0';
        }
        strncpy(req->hash, hash_val, BLOCKSIZE);
        req->type = REGDIR;
        
        // Send the request to the server
        if (write(*sock_fd, &(req->type), sizeof(int)) != sizeof(int) ||
            write(*sock_fd, req->path, sizeof(req->path)) != MAXPATH ||
            write(*sock_fd, &(mode), sizeof(int)) != sizeof(int) ||
            write(*sock_fd, req->hash, sizeof(req->hash)) != BLOCKSIZE ||
            write(*sock_fd, &(size), sizeof(int)) != sizeof(int)) {
            fprintf(stderr, "Error encountered while copying %s: write\n", basename_relative_path);
            return 1;
        }
        
        // Free the memory allocated by the request struct
        free(hash_val);
        free(req);
        
        // Wait for the server's response
        // First, we prepare to listen to multiple
        // file descriptors by initializing a set of file descriptors.
        int max_fd = *sock_fd;
        fd_set all_fds, listen_fds;
        FD_ZERO(&all_fds);
        FD_SET(*sock_fd, &all_fds);
        int response = -1;
    
        while (response == -1) {
            // select updates the fd_set it receives, so we always use a copy and retain the original.
            listen_fds = all_fds;
            int nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
            if (nready == -1) {
                fprintf(stderr, "Error encountered while copying %s: client: select\n", basename_relative_path);
            }
        
            // If the server is ready for reading ...
            if (FD_ISSET(*sock_fd, &listen_fds)) {
                int num_read = read(*sock_fd, &response, sizeof(int));
                if (num_read != sizeof(int)) {
                    printf("5");
                    fprintf(stderr, "Error encountered while copying %s: read\n", basename_relative_path);
                    return 1;
                }
            }
        }
        
        // If the files are incompatible
        if (response == ERROR) {
            fprintf(stderr, "Error encountered while copying %s: server ERROR\n", basename_relative_path);
            return 1;    
        } else {
            // Otherwise, open the directory for reading
            DIR *dirp = opendir(source);
            if (dirp == NULL) {
                fprintf(stderr, "Error encountered while copying %s: opendir\n", basename_relative_path);
                return 1;
            }
            
            struct dirent *dp = malloc(sizeof(struct dirent));
            
            // Skip files that start with "."
            if ((dp->d_name)[0] != '.') {
                // Get filepath relative to the basename directory
                char new_path[MAXPATH];
                strncpy(new_path, basename_relative_path, MAXPATH);
                strncat(new_path, "/", MAXPATH - strlen(req->path));
                strncat(new_path, dp->d_name, MAXPATH - strlen(req->path) - 1);
                
                // Get source path
                char new_source[MAXPATH];
                strncpy(new_source, source, MAXPATH);
                strncat(new_source, "/", MAXPATH - strlen(source));
                strncat(new_source, dp->d_name, MAXPATH - strlen(source) - 1);
                
                // Copy the file
                error = copy_file(new_source, new_path, sock_fd, server);
            }
        }
    }
    
    // If error has been set to 1, then
    // an error was encountered during file transfer
    // Otherwise, no errors were occurred
    return error;
}

void rcopy_server(unsigned short port) {
	int client_fd;
	struct client *head = NULL;
	struct client *p;
	struct sockaddr_in q;
	socklen_t len;
	int nready;
	
	// Create the socket FD, bind and listen
    int sock_fd = bindandlisten();
	
	// The client data transfer accept loop. First, we prepare to listen to multiple
    // file descriptors by initializing a set of file descriptors.
    int max_fd = sock_fd;
    fd_set all_fds, listen_fds;
    FD_ZERO(&all_fds);
    FD_SET(sock_fd, &all_fds);

    while (1) {
        // select updates the fd_set it receives, so we use a copy and retain the original.
        listen_fds = all_fds;
        
		nready = select(max_fd + 1, &listen_fds, NULL, NULL, NULL);
		if (nready == 0) {
			printf("No response from clients");
		}
        else if (nready == -1) {
            perror("server: select");
            continue;
        }

        // Is it the original socket? Create a new connection ...
        if (FD_ISSET(sock_fd, &listen_fds)) {
			len = sizeof(q);
            if((client_fd = accept(sock_fd, (struct sockaddr *)&q, &len)) < 0) {
				perror("server: accept");
				exit(1);
			}
			FD_SET(client_fd, &all_fds);
            if (client_fd > max_fd) {
                max_fd = client_fd;
            }
            printf("Accepted connection\n");
			head = addclient(head, client_fd, q.sin_addr);
        }

        // Next, check the clients.
        for (int i = 0; i <= max_fd; i++) {
			if (FD_ISSET(i, &listen_fds)) {
				printf("isset");
				for (p = head; p != NULL; p = p->next) {
					if (p->fd == i) {
						printf("handleclient");
						int result = handleclient(p, head);
						if (result == -1) {
							int tmp_fd = p->fd;
							head = removeclient(head, tmp_fd);
							FD_CLR(tmp_fd, &all_fds);
							printf("Closed a connection\n");
							close(tmp_fd);
						}
						break;
					}
				}
			}
        }
    }
};

// the function for handling the client, with a switch statement for each possible input state
// in each state, update the state
int handleclient(struct client *p, struct client *top) {
	int read_result;
	switch(p->state) {
		// read in the request type and store it in the request struct in the corresponding client struct
		case AWAITING_TYPE :
		{
			
			int type;
			if ((read_result = read(p->fd, &type, sizeof(int))) < 0) {
				perror("read");
				return -1;
			}
			
			type = ntohl(type);
			p->req->type = type;
			p->state = AWAITING_PATH;
			break;
		// read in the path and store
		}
		case AWAITING_PATH :
		{
			printf("path\n");
			char buf[MAXPATH];
			if ((read_result = read(p->fd, buf, MAXPATH)) < 0) {
				perror("read");
				return -1;
			}
			
			strncpy(p->req->path, buf, MAXPATH);
			p->state = AWAITING_PERM;
			break;
		}
		// read in the mode and store
		case AWAITING_PERM :
		{
			printf("perm\n");
			mode_t mode;
			if ((read_result = read(p->fd, &mode, sizeof(mode))) < 0) {
				perror("read");
				return -1;
			}
			
			mode = ntohl(mode);
			p->req->mode = mode;
			p->state = AWAITING_HASH;
			break;
		}
		// read in the hash and store
		case AWAITING_HASH :
		{
			printf("hash\n");
			char hash[BLOCKSIZE];
			if ((read_result = read(p->fd, hash, BLOCKSIZE)) < 0) {
				perror("read");
				return -1;
			}
			
			strncpy(p->req->hash, hash, BLOCKSIZE);
			p->state = AWAITING_SIZE;
			break;
		}
		// read in the size and store
		case AWAITING_SIZE :
		{
			printf("size\n");
			int size;
			int signal;
			if ((read_result = read(p->fd, &size, sizeof(int))) < 0) {
				perror("read");
				return -1;
			}
			
			size = ntohl(size);
			p->req->size = size;
			// if the client is not the main client, the next state should be AWAITING_DATA
			if (p->req->type == TRANSFILE) {
				p->state = AWAITING_DATA;
			} else {
				// otherwise set the next state back to the first, AWAITING_TYPE
				p->state = AWAITING_TYPE;
				// the stat struct for the file or directory in dest
				struct stat *buf = malloc(sizeof(struct stat));
				int lst = lstat(p->req->path, buf);
				int same = check_same(p->req, lst, buf);
				// the files are the same, update the permissions
				if (same == 0) {
					if (chmod(p->req->path, p->req->mode & 0777) == -1) {
						perror("server: chmod");
						return -1;
					}
					// send an OK signal
					signal = OK;
					if (write(p->fd, &signal, sizeof(int)) < 0) {
						perror("server: write");
						return -1;
					}
				// the files are different, send the SENDFILE signal
				} else if (same == 1) {
					signal = SENDFILE;
					if (write(p->fd, &signal, sizeof(int)) < 0) {
						perror("server: write");
						return -1;
					}
				// the path is a directory that doesn't already exist
				} else if (same == 2) {
					// make the directory with the correct permissions
					int mkd = 0;
					if ((mkd = mkdir(p->req->path, buf->st_mode & 0777)) == -1) {
						perror("mkdir");
						return -1;
					}
					// send the OK signal
					signal = OK;
					if (write(p->fd, &signal, sizeof(int)) < 0) {
						perror("server: write");
						return -1;
					}
				// the file types are incompatible
				} else {
					signal = ERROR;
					if (write(p->fd, &signal, sizeof(int)) < 0) {
						fprintf(stderr, "File type of %s incompatible\n", p->req->path);
						return -1;
					}
				}
				free(buf);
			}
			break;
		}
		// the request type is TRANSFILE
		case AWAITING_DATA :
		{
			printf("data\n");
			char data[MAXDATA];
			// read in the transmitted file data
			if (read(p->fd, data, MAXDATA) < MAXDATA) {
				perror("server: fread");
				return -1;
			}
			// open a file for writing
			FILE *copy = fopen(p->req->path, "w");
			if (copy == NULL) {
				perror("server: fopen");
				return -1;
			}
			// write the transmitted data
			if (fwrite(data, 1, MAXDATA, copy) < MAXDATA) {
				perror("server: write");
				return -1;
			}
			// set the file permissions
			if ((chmod(p->req->path, p->req->mode & 0777)) == -1) {
				perror("server: chmod");
				return -1;
			}
			// close the file for writing
			if (fclose(copy) != 0) {
				perror("fclose");
				return -1;
			}
			return -1;
		}
	}
	return 0;
};

// the function for deciding the response to the client's request
int check_same(struct request *request, int lst, struct stat *buf) {
	int ret = 0;
	char *stream_hash = malloc(BLOCKSIZE);
	// the case where the file already exists but file types are incompatible
	if ((lst == 0 && (S_ISDIR(buf->st_mode) && request->type == REGFILE)) ||
		(S_ISREG(buf->st_mode) && request->type == REGDIR)) {
		ret = -1;
	}
	// the case where the request is for a directory which doesn't already exist
	if (lst == -1 && S_ISDIR(buf->st_mode)) {
		ret = 2;
	}
	// the case where the file already exists but is different
	FILE *stream = fopen(request->path, "r");
	if (stream == NULL) {
		perror("server: fopen");
	}
	if (lst == -1 || buf->st_size != request->size || 
		(stream != NULL && check_hash(((char *)request->hash), hash(stream_hash, stream)))) {
		ret = 1;
	}
	if ((fclose(stream)) != 0) {
		perror("server: fclose");
	}
	free(stream_hash);
	// there is no need to transmit the file data if none of the if statements were entered
	return ret;
}

static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
	struct client *p = malloc(sizeof(struct client));
	if (!p) {
		perror("server: malloc");
		exit(1);
	}
	
	printf("Adding client %s\n", inet_ntoa(addr));
	
	p->fd = fd;
	p->ipaddr = addr;
	p->state = AWAITING_TYPE;
	p->next = top;
	top = p;
	return top;
}

static struct client *removeclient(struct client *top, int fd) {
	struct client **p;

    for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
    return top;
}

 /* bind and listen, abort on error
  * returns FD of listening socket
  */
int bindandlisten(void) {
    struct sockaddr_in r;
    int listenfd;

	// Create the socket FD
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
	
	// Make sure we can reuse the port immediately after the
    // server terminates. Avoids the "address in use" error
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(PORT);

	// Bind the selected port to the socket.
    if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }

	// Announce willingness to accept connections on this socket.
    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}