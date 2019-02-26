#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAXNAME 80  /* maximum permitted name size, not including \0 */
#define NPITS 6  /* number of pits on a side, not including the end pit */
#define NPEBBLES 4 /* initial number of pebbles per pit */
#define MAXMESSAGE (MAXNAME + 50) /* maximum permitted message size */

int port = 3000;
int listenfd;

struct player {
    int fd;
    char name[MAXNAME+1]; 
    int pits[NPITS+1];  // pits[0..NPITS-1] are the regular pits 
                        // pits[NPITS] is the end pit
    int name_status; // 1 if the player's name is done, otherwise 0
    int game_status; // 1 if the player has acceptable name, otherwise 0
    int turn; // 1 if it is the player's turn to move, otherwise 0
    struct player *next;
};
struct player *playerlist = NULL;


extern int check_name(char *s);
extern void delete_player(int deletefd);
extern int play_game(struct player *p, int n);
extern void pass_turn(struct player *p);
extern void write_your_move(struct player *p);
extern void write_statement();
extern void parseargs(int argc, char **argv);
extern void makelistener();
extern int compute_average_pebbles();
extern int game_is_over();  /* boolean */
extern void broadcast(char *s);


int main(int argc, char **argv) {
    char msg[MAXMESSAGE];

    parseargs(argc, argv);
    makelistener();

    int maxfd = listenfd;
    fd_set all_fds;

    FD_ZERO(&all_fds);
    FD_SET(listenfd, &all_fds);

    while (!game_is_over()) {
        fd_set read_fds = all_fds;
        int nready = select(maxfd + 1, &read_fds, NULL, NULL, NULL);
        if (nready == -1) {
            perror("server: select");
            exit(1);
        }
        /* add new connected player to playerlist and send welcome message */
        if (FD_ISSET(listenfd, &read_fds)) {
            struct player *new_player = malloc(sizeof(struct player));

            new_player->name[0] = '\0';
            new_player->name_status = 0;
            new_player->game_status = 0;
            new_player->turn = 0;
            int playerfd = accept(listenfd, NULL, NULL);
            new_player->fd = playerfd;
            if (playerfd > maxfd) {
                maxfd = playerfd;
            }
            FD_SET(playerfd, &all_fds);
            int pebble_num = compute_average_pebbles();
            for (int i = 0; i < NPITS; i++) {
                new_player->pits[i] = pebble_num;
            }
            /* add to playerlist */
            new_player->next = playerlist;
            playerlist = new_player;
            char welcomemsg[MAXMESSAGE];
            snprintf(welcomemsg, MAXMESSAGE, "Welcome to Mancala. What is your name?\r\n");
            write(new_player->fd, welcomemsg, strlen(welcomemsg));
        }

        for (struct player *p = playerlist; p; p = p->next) {
            /* player who writes */
            if (FD_ISSET(p->fd, &read_fds)) {
                /* split the case into incomplete and complete name */
                if (p->name_status == 0) {
                    /* incomplete name */
                    char name[MAXNAME + 1];
                    int read_ret = read(p->fd, &name, MAXNAME);
                    if (read_ret == 0) {
                        /* player disconnected */
                        delete_player(p->fd);
                        FD_CLR(p->fd, &all_fds);
                        free(p);
                        printf("Unnamed player disconnected.\n");
                    } else if (read_ret > 0) {
                        /* player connects successfully */
                        /* check if name includes newline conventions */
                        for (int i = 0; i < strlen(name); i ++) {
                            if (name[i] == '\r' || name[i] == '\n') {
                                name[i] ='\0';
                                p->name_status = 1;
                            }
                        }
                        if (strlen(name) + strlen(p->name) > MAXNAME) {
                            /* disconnect the player if name is too long */
                            delete_player(p->fd);
                            FD_CLR(p->fd, &all_fds);
                            close(p->fd);
                            free(p);
                            printf("Player disconnected: name is too long.\n");
                        } else {
                            strncat(p->name, name, MAXNAME - strlen(p->name) - 1);
                            if (p->name_status == 1 && p->game_status == 0) {
                                /* name is not checked yet */
                                if (check_name(p->name)) {
                                    /* name accepted */
                                    p->game_status = 1;
                                    char newplayer_msg[MAXMESSAGE];
                                    printf("%s joins game.\n", p->name);
                                    snprintf(newplayer_msg, MAXMESSAGE, "%s joins game.\r\n", p->name);
                                    broadcast(newplayer_msg);
                                    write_statement();
                                    /* set the first acceptable player's turn to move */
                                    if (playerlist != NULL) {
                                        int num_accptable = 0;
                                        for (struct player *allplayer = playerlist; allplayer; allplayer = allplayer->next) {
                                            if (allplayer->game_status == 1) {
                                                num_accptable ++;
                                            }
                                        }
                                        if (num_accptable == 1) {
                                            p->turn = 1;
                                            write_your_move(p);
                                        }
                                    }
                                } else {
                                    /* name is not acceptable, disconnect */
                                    delete_player(p->fd);
                                    FD_CLR(p->fd, &all_fds);
                                    close(p->fd);
                                    free(p);
                                    printf("Player disconnected: duplicated name.\n");
                                }
                            }
                        }
                    } else {
                        perror("read");
                        exit(1);
                    }
                } else if (p->name_status == 1 && p->game_status == 1) {
                    /* name accepted and already joined game */ 
                    if (p->turn == 1) {
                        /* the player should move */
                        char num_in[MAXMESSAGE];

                        int ret = read(p->fd, &num_in, MAXMESSAGE);
                        if (ret == 0) {
                            /* player disconnect when it is their turn */
                            pass_turn(p);
                            printf("Player %s disconnected.\n", p->name);
                            delete_player(p->fd);
                            FD_CLR(p->fd, &all_fds);
                            free(p);
                        } else if (ret > 0) {
                            /* player writes move */
                            int move = strtol(num_in, NULL, 10);
                            if (move >= 0 && move < NPITS && p->pits[move] != 0) {
                                /* valid move */
                                printf("%s move number %d pit.\n", p->name, move);
                                int play_again = play_game(p, move);
                                if (play_again == 0) {
                                    p->turn = 0;
                                    pass_turn(p);
                                } else {
                                    /* player gets another turn */
                                    write_your_move(p);
                                }
                            } else {
                                /* invalid move */
                                char invalid[MAXMESSAGE];
                                snprintf(invalid, MAXMESSAGE, "Invalid move, please enter another move.\r\n");
                                write(p->fd, invalid, strlen(invalid));
                            }
                        } else {
                            perror("read");
                            exit(1);
                        }
                    } else if (p->turn == 0) {
                        /* player types something when it is not their turn, or they disconnect */
                        char whatever[MAXMESSAGE];
                        char warningmsg[MAXMESSAGE];
                        char discon_msg[MAXMESSAGE];
                        int ret = read(p->fd, &whatever, MAXMESSAGE);
                        if (ret == 0){
                            /* player disconnects */
                            printf("Player %s disconnected.\n", p->name);
                            snprintf(discon_msg, MAXMESSAGE, "Player %s disconnected.\r\n", p->name);
                            broadcast(discon_msg);
                            delete_player(p->fd);
                            FD_CLR(p->fd, &all_fds);
                            free(p);
                        } else if (ret > 0) {
                            /* naughty player */
                            snprintf(warningmsg, MAXMESSAGE, "It is not your turn.\r\n");
                            if (write(p->fd, warningmsg, strlen(warningmsg)) < 0) {
                                perror("write");
                                exit(1);
                            }
                        } else {
                            perror("read");
                            exit(1);
                        }
                    }
                }
                break;
            }
        }
    }

    broadcast("Game over!\r\n");
    printf("Game over!\n");
    for (struct player *p = playerlist; p; p = p->next) {
        int points = 0;
        for (int i = 0; i <= NPITS; i++) {
            points += p->pits[i];
        }
        printf("%s has %d points\r\n", p->name, points);
        snprintf(msg, MAXMESSAGE, "%s has %d points\r\n", p->name, points);
        broadcast(msg);
    }

    return 0;
}


/* return 1 if name is acceptable, otherwise return 0 */
int check_name(char *s) {
    int same_count = 0;
    if (strlen(s) == 0) {
        return 0;
    }
    for (struct player *p = playerlist; p; p = p->next) {
        if (strcmp(p->name, s) == 0) {
            same_count ++;
        }
    }
    if (same_count == 1) {
        return 1;
    }
    return 0;
}


/* delete the given fd player in playerlist */
void delete_player(int deletefd) {
    struct player *new = playerlist;
    if (new->fd == deletefd) {
        // if only one player, clean the playerlist
        playerlist = playerlist->next;
    } else {
        for (; new; new = new->next) {
            if (new->next != NULL && new->next->fd == deletefd) {
                new->next = (new->next)->next;
                break;
            } 
        }
    }
}


/* play game from player p with number pit n, and return 1 if the player get another turn */
int play_game(struct player *p, int n) {
    int flag = 0;
    struct player *cur =p;
    int num = cur->pits[n];
    cur->pits[n] = 0;
    int i = 0;
    int position = n + 1;
    while (i != num) {
        if (position >= 0 && position <= NPITS) {
            cur->pits[position] += 1;
            i += 1;
            position += 1;
        } else {
            position = 0;
            if (cur->next != NULL) {
                /* player has next */
                while (cur->next->game_status != 1) {
                    if (cur->next->next != NULL) {
                        cur = cur->next;
                    } else {
                        cur = playerlist;
                    }
                }
                /* cur->next is in the game */
                cur = cur->next;
            } else {
                cur = playerlist;
                while (cur->game_status != 1) {
                    cur = cur->next;
                }
            }
        }
    }
    if (strcmp(cur->name, p->name) == 0 && position - 1 == NPITS) {
        flag = 1;
    }
    write_statement();
    for (struct player *everyone = playerlist; everyone; everyone = everyone->next) {
        if (everyone->turn == 0 && everyone->game_status == 1) {
            char name_movemsg[MAXMESSAGE];
            snprintf(name_movemsg, MAXMESSAGE, "%s moves number %d pit.\r\n", p->name, n);
            if (write(everyone->fd, name_movemsg, strlen(name_movemsg)) != strlen(name_movemsg)) {
                perror("write");
                exit(1);
            }
        }
    }
    return flag;
}


/* pass the moving turn to the next player in game and write move reminder message */
void pass_turn(struct player *p) {
    struct player *cur = p;
    if (cur->next != NULL) {
        while (cur->next->game_status != 1) {
            if (cur->next != NULL) {
                cur = cur->next;
            } else {
                cur = playerlist;
            }
        }
        cur->next->turn = 1;
        write_your_move(cur->next);
    } else {
        cur = playerlist;
        while (cur->next && cur->game_status != 1) {
            cur = cur->next;
        }
        if (cur->game_status == 1){
            cur->turn = 1;
            write_your_move(cur);
        }
    }
}


/* write "Your move?", and tells everyone else whose turn it is */
void write_your_move(struct player *p) {
    char *movemsg = "Your move?\r\n";
    if (write(p->fd, movemsg, strlen(movemsg)) != strlen(movemsg)){
        perror("write");
        exit(1);
    }
    for (struct player *all = playerlist; all; all = all->next) {
        if (all->turn == 0 && all->game_status == 1) {
            char othersmsg[MAXMESSAGE];
            snprintf(othersmsg, MAXMESSAGE, "It's %s's move.\r\n", p->name);
            if (write(all->fd, othersmsg, strlen(othersmsg)) != strlen(othersmsg)) {
                perror("write");
                exit(1);
            }
        }
    }
}


void write_statement() {
    char movemsg[MAXMESSAGE];
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->game_status == 1) {
            snprintf(movemsg, MAXMESSAGE, "%s: [0]%d [1]%d [2]%d [3]%d [4]%d [5]%d [end pit]%d\n", p->name, p->pits[0], p->pits[1], p->pits[2], p->pits[3], p->pits[4], p->pits[5], p->pits[6]);
            broadcast(movemsg);
        }
    }
}


void parseargs(int argc, char **argv) {
    int c, status = 0;
    while ((c = getopt(argc, argv, "p:")) != EOF) {
        switch (c) {
        case 'p':
            port = strtol(optarg, NULL, 0);  
            break;
        default:
            status++;
        }
    }
    if (status || optind != argc) {
        fprintf(stderr, "usage: %s [-p port]\n", argv[0]);
        exit(1);
    }
}


void makelistener() {
    struct sockaddr_in r;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
               (const char *) &on, sizeof(on)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *)&r, sizeof(r))) {
        perror("bind");
        exit(1);
    }

    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
}



/* call this BEFORE linking the new player in to the list */
int compute_average_pebbles() { 
    struct player *p;
    int i;

    if (playerlist == NULL) {
        return NPEBBLES;
    }

    int nplayers = 0, npebbles = 0;
    for (p = playerlist; p; p = p->next) {
        nplayers++;
        for (i = 0; i < NPITS; i++) {
            npebbles += p->pits[i];
        }
    }
    return ((npebbles - 1) / nplayers / NPITS + 1);  /* round up */
}


int game_is_over() { /* boolean */
    int i;

    if (!playerlist) {
       return 0;  /* we haven't even started yet! */
    }

    for (struct player *p = playerlist; p; p = p->next) {
        int is_all_empty = 1;
        for (i = 0; i < NPITS; i++) {
            if (p->pits[i]) {
                is_all_empty = 0;
            }
        }
        if (is_all_empty) {
            return 1;
        }
    }
    return 0;
}


void broadcast(char *s) {
    for (struct player *p = playerlist; p; p = p->next) {
        if (p->game_status == 1) {
            /* player in game which has acceptable name */
            if (write(p->fd, s, strlen(s)) != strlen(s)) {
                perror("write");
                exit(1);
            }
        }
    }
}
