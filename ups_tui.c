#ifdef __APPLE__
    #include <curses.h>
#else
    #include <ncurses.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

/*
 * UPS Popup Notification System (Call by ups_monitor_daemon)
 * 
 * A C/ncurses-based popup warning system for UPS status.
 * Displays emergency notifications when power events occur:
 *   - Critical battery shutdown warning
 *   - Mains power loss notification
 *   - Mains power restoration confirmation
 * 
 * Features:
 *   - Centered, colored popup windows with borders
 *   - Countdown timers for automatic closure
 *   - Signal handling for clean termination
 *   - Responsive to terminal resizing
 *   - Non-blocking input for user interaction
 * 
 * Build:
 *   - For Mac: gcc -o ups_tui ./ups_tui.c -lcurses
 *   - For Linux: gcc -o ups_tui ups_tui.c -lncurses
 * 
 * Test commands:
 * ./ups_tui --type 1 --battery 10 --timer 60    # Critical battery (type 1)
 * ./ups_tui --type 2 --battery 75               # Mains lost (type 2)
 * ./ups_tui --type 3 --battery 100              # Mains restored (type 3)
 */

#define BOX_HEIGHT 14
#define BOX_WIDTH 50

// Global variable to indicate when to stop the popup
volatile sig_atomic_t stop_popup = 0;

void signal_handler(int signum) {
    stop_popup = 1;

    /* Simple write to stderr because curses might have taken over terminal */
    const char *msg = "\nReceived stop signal, closing popup...\n";
    write(STDERR_FILENO, msg, strlen(msg));
}

/* Set up signal handlers */
void setup_signal_handlers(void) {
    struct sigaction sa;

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    /* Set signal handlers */
    sigaction(SIGTERM, &sa, NULL);  /* Termination signal (kill) */
    sigaction(SIGINT, &sa, NULL);   /* Ctrl+C */
    sigaction(SIGUSR1, &sa, NULL);  /* Custom user signal */
}

int center_x(int start_x, const char *text) {
    return start_x + (BOX_WIDTH - strlen(text)) / 2;
}

void draw_centered_popup(WINDOW *stdscr, int timer, int msgType, int batteryPercent)
{
    int height, width;
    getmaxyx(stdscr, height, width);

    const char *titles[] = {
        "UPS battery critically low", 
        "Mains power lost. UPS is now on battery", 
        "Mains power has been restored"
    };

    const char *instructions[] = {
        "Save your work immediately.",
        "Preparation needed if outage persists.",      
        "No further action required."
    };

    if (height < BOX_HEIGHT || width < BOX_WIDTH) {
        attron(COLOR_PAIR(1));
        mvaddstr(0, 0, "Terminal too small! Resize to view warning.");
        attroff(COLOR_PAIR(1));
        return;
    }

    int start_y = (height / 2) - (BOX_HEIGHT / 2);
    int start_x = (width / 2) - (BOX_WIDTH / 2);

    /* Fill box area with spaces */
    attron(COLOR_PAIR(msgType + 1));
    for (int i = 0; i < BOX_HEIGHT; ++i) {
        for (int j = 0; j < BOX_WIDTH; ++j) {
            mvaddch(start_y + i, start_x + j, ' ');
        }
    }

    /* Draw horizontal lines */
    mvhline(start_y, start_x, ACS_HLINE, BOX_WIDTH);
    mvhline(start_y + BOX_HEIGHT - 1, start_x, ACS_HLINE, BOX_WIDTH);
    /* Draw vertical lines */
    mvvline(start_y, start_x, ACS_VLINE, BOX_HEIGHT);
    mvvline(start_y, start_x + BOX_WIDTH - 1, ACS_VLINE, BOX_HEIGHT);
    /* Draw Corners */
    mvaddch(start_y, start_x, ACS_ULCORNER);
    mvaddch(start_y, start_x + BOX_WIDTH - 1, ACS_URCORNER);
    mvaddch(start_y + BOX_HEIGHT - 1, start_x, ACS_LLCORNER);
    mvaddch(start_y + BOX_HEIGHT - 1, start_x + BOX_WIDTH - 1, ACS_LRCORNER);

    const char *title = titles[msgType - 1];
    int centre_x = center_x(start_x, title);
    attron(A_BOLD);
    mvaddstr(start_y + 3, centre_x, title);
    attroff(A_BOLD);

    if (msgType == 1) {
        /* Type 1: Critical battery shutdown */
        char countdown_text[100];
        snprintf(countdown_text, sizeof(countdown_text), "SHUTDOWN IN %d SECONDS", timer);

        int attr = COLOR_PAIR(msgType + 1) | A_BOLD;
        if (timer % 2 != 0) {
            attr |= A_REVERSE;
        }

        attron(attr);
        mvaddstr(start_y + 7, center_x(start_x, countdown_text), countdown_text);
        attroff(attr);
    } else {
        /* Type 2 and 3: Warning/Info messages */
        char msg1[100], msg2[100];
        snprintf(msg1, sizeof(msg1), "AUTO Close in %d seconds", timer);
        snprintf(msg2, sizeof(msg2), "Current Battery: %d%%", batteryPercent);

        attron(A_BOLD);
        mvaddstr(start_y + 6, center_x(start_x, msg1), msg1);
        mvaddstr(start_y + 9, center_x(start_x, msg2), msg2);
        attroff(A_BOLD);
    }

    const char *instruction = instructions[msgType - 1];
    int instruction_x = center_x(start_x, instruction);
    mvaddstr(start_y + 11, instruction_x, instruction);
    attroff(COLOR_PAIR(msgType + 1));
}

int main(int argc, char *argv[]) {
    int msg_type = 0;
    int battery = 0;
    int timer = 3;

    /* Simple argument parsing */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            msg_type = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--battery") == 0 && i + 1 < argc) {
            battery = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timer") == 0 && i + 1 < argc) {
            timer = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Usage: %s --type <1-3> --battery <0-100> [--timer <seconds>]\n", argv[0]);
            return 1;
        }
    }

    /* Validate required arguments */
    if (msg_type < 1 || msg_type > 3 || battery < 0 || battery > 100) {
        fprintf(stderr, "Invalid arguments. Type must be 1-3, battery must be 0-100\n");
        return 1;
    }

    setup_signal_handlers();

    /* Initialise curses */
    initscr();
    cbreak();     // Immediate key response
    noecho();     // Don't echo typed characters
    curs_set(0);  // Hide cursor

    /* Check if terminal supports colour */
    if (has_colors() == FALSE) {
        endwin();
        fprintf(stderr, "Your terminal does not support colour\n");
        return 1;
    }
    /* Initialise colours */
    start_color();
    init_pair(1, COLOR_RED, COLOR_BLACK);  
    init_pair(2, COLOR_WHITE, COLOR_RED);  
    init_pair(3, COLOR_WHITE, COLOR_YELLOW);  
    init_pair(4, COLOR_BLACK, COLOR_GREEN);
    
    timeout(100); // 100ms timeout for getch()
    
    int original_timer = timer;
    /* Main loop */
    while( timer > 0 && !stop_popup) 
    {
        clear();
        draw_centered_popup(stdscr, timer, msg_type, battery);
        refresh();

        // Wait for 1 second (in 100ms increments to check stop_popup)
        for (int i = 0; i < 10; ++i) {
            if (stop_popup) break;
            usleep(100000); // 100ms
        }

        timer--;
    }

    // Clean up
    clear();
    refresh();
    endwin();

    return 0;
}