#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h> 
#include <unistd.h>  
#include <termios.h>

#define DEBUG_ENABLED 1


#if DEBUG_ENABLED
    FILE* debugFile;
    #define DEBUG(format, ...) fprintf (debugFile, format, __VA_ARGS__)
#else
    #define DEBUG
#endif

#define CAPACIY_INCREMENT 10

struct termios termios_config;

char* makeStr(char* string, int length){
    char* new = malloc(sizeof(char) * (length+1));
    memcpy(new, string, length);
    string[length] = '\0';
    return string;
}

typedef struct{
    char **commands;
    int length;
    int capacity;
}commandHistory;
// History should be linked list !!!!
void historyInit(commandHistory *h){
    h->length = 0;
    h-> capacity = CAPACIY_INCREMENT;
    h->commands = malloc(sizeof(char*) * CAPACIY_INCREMENT);
}
void historyAdd(commandHistory *h, char *command){
    if(h->length == h->capacity){
        h->capacity += CAPACIY_INCREMENT;
        h->commands = realloc(h->commands, sizeof(char*) * h->capacity);
    }
    h->commands[h->length++] = command;
}

typedef struct{
    commandHistory history;
    int pos; // -1 if current command is not loaded from history otherwise last loaded command's index in history
    char *content;
    int length; // length of current content
    int capacity;
    int curPos;
    char* draft;
}shellState;

void stateInit(shellState *l){
    l->content = malloc(sizeof(char) * CAPACIY_INCREMENT);
    l->length = 0;
    l->capacity = CAPACIY_INCREMENT;
    l->curPos = 0;
    l->pos = -1;
    l->draft = 0;
    historyInit(&l->history);
}

void loadFromHistory(shellState *l, int i){
    int s = 0;
    char* record = l->history.commands[i];
    
    if(l->pos == -1 && l->length > 0) l->draft = makeStr(l->content, l->length);
    
    if(l->curPos > 0) printf("\033[%dD\x1b[K", l->curPos);
    
    l->curPos = l->length = printf("%s", record);
    if(l->curPos > l->capacity){
        free(l->content);
        l->content = malloc(sizeof(char)*l->length);
    }
    memcpy(l->content, record, l->length);
    l->pos = i;
}

void loadPrevious(shellState *l){
    if(l->pos > 0){ //We won't do anything in case it is 0 since it is oldest record in history
        loadFromHistory(l, l->pos-1);
    }else if(l->pos == -1 && l->history.length > 0){
        loadFromHistory(l, l->history.length-1);
    }
}
void loadNext(shellState *l){
    if(l->pos < 0) return;
    if(++l->pos < l->history.length){
        loadFromHistory(l, l->pos);
    }else{
        if(l->draft){
            if(l->content) free(l->content);
            l->content = l->draft;
            l->draft = 0;
            if(l->curPos > 0) printf("\033[%dD\x1b[K", l->curPos);
            l->length = l->curPos = printf("%s", l->content);
            l->capacity = l->curPos+1;
        }else{
            if(l->curPos > 0) printf("\033[%dD\x1b[K", l->curPos);
            l->length = 0;
            l->curPos = 0;
        }
        l->pos = -1;
    }
}

void addChar(shellState *l, char ch){
    if(l->curPos == l->length) l->length++;
    if(l->length > l->capacity){
        l->capacity += CAPACIY_INCREMENT;
        l->content = realloc(l->content, sizeof(char) * l->capacity );
    }
    l->content[l->curPos++] = ch;
    putchar(ch);
}

// When curser is over a multibyte utf8 char curPos will point first char of that
// So regardless of length of char first we decrease curPos by one
// Then we check if we have stepped over a multibyte char and decrease until curPos points first byte of character
void moveBackward(shellState *l){
    if(l->curPos > 0){
        printf("\033[1D");
        l->curPos--;

        //Check if we are pointing at a 1+th byte of multibyte character
        while((l->content[l->curPos] & 0xA0) == 0x80){
            l->curPos--;
        }
        
    }
}

void moveForward(shellState *l){
    if(l->curPos < l->length){
        printf("\033[1C");
        //l->curPos++;
        while((l->content[l->curPos++] & 0xA0) == 0x80);
    }
}
void backspace(shellState *l){
    int i, j;
    if(l->curPos > 0){
        write(STDOUT_FILENO, "\033[1D\x1b[K", 7);
        l->curPos--;
        l->length--;
        i = l->length - l->curPos;
        for(j = 0; j < i ; j++){
            l->content[l->curPos+j] = l->content[l->curPos+j+1];
            putchar(l->content[l->curPos+j]);
        }
        if(i>0) printf("\033[%dD", i);
    }
}
void delete(shellState *l){
    int i, j;
    if(l->curPos < l->length){
        write(STDOUT_FILENO, "\x1b[K", 3);
        i = l->length - l->curPos;
        for(j = 0; j < i ; j++){
            l->content[l->curPos+j] = l->content[l->curPos+j+1];
            putchar(l->content[l->curPos+j]);
        }
        if(i>1) printf("\033[%dD", i-1);
        l->length--;
    }
}

void commit(shellState *l){
    if(l->length > 0){
        if(l->capacity == l->length || l->capacity/l->length > 1.04){
            l->content = realloc(l->content, sizeof(char) * (l->length+1));
        }
        l->content[l->length] = '\0';
        printf("\n\r%s.", l->content);
        historyAdd(&l->history, l->content);

        l->content = malloc(sizeof(char) * CAPACIY_INCREMENT);
        l->length = 0;
        l->capacity = CAPACIY_INCREMENT;
        l->curPos = 0;
    }else{
        l->curPos = 0;
    }
    l->pos = -1;
}


void runCommand(char command[]){
    char buffer[0];
    char** args;
    args = malloc(sizeof(char*)*CAPACIY_INCREMENT);

}

void processCommand(char command[]){
    pid_t pid;
    pid = fork();
    if(pid == -1){
        printf("Fork Error! Command failed to process!\n\r");
    }
    if(pid == 0){ // Child
        runCommand(command);
        exit(0);
    }
}

void disableRawMode(){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios_config);
}

void enableRawMode(){
    tcgetattr(STDIN_FILENO, &termios_config);
    struct termios raw = termios_config;
    // ECHO : Echo input characters.
    // ICANON : Enable canonical mode
    // ISIG : When any of the characters INTR, QUIT, SUSP, or DSUSP are received, generate the corresponding signal.
    // IEXTEN : 
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // Disabling all those above.
    // ICRNL: Translate carriage return to newline on input
    // IXON: CTRL-S and CTRL-Q
    raw.c_iflag &= ~(ICRNL | IXON); // Disable those
    raw.c_oflag &= ~(OPOST);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(int argc, char *argv[]){
    char** history;
    int historyCapacity;
    int historyI = 0;
    char buffer[150];
    int i = 0;
    int quote = 0;
    int escape = 0;
    int escapeSequence = 0;
    char ch;
    int control = 1;
    int processed = 0;
    
    #if DEBUG_ENABLED
        debugFile = fopen("debug.txt", "w+");
    #endif
    DEBUG("test : %d", 123);
    shellState state;
    shellState *curLinePtr = &state;
    stateInit(curLinePtr);

    //system("stty raw");
    //system("stty -echo");
    enableRawMode();
    system("clear");
    write(STDOUT_FILENO, "\x1b[2J", 4);
    printf("Welcome\n\rPress CTRL-D to quit\n\r<>");

    while(control){
        ch = getchar();
        processed = 0;

        if(escapeSequence == 3){
            if(ch == '~'){
                delete(curLinePtr);
                processed = 1;
            }else{
                processed = 0;
            }
            escapeSequence = 0;
        }else if(escapeSequence == 2){
            processed = 1;
            switch (ch)
            {
                case '3':
                    escapeSequence++;
                    break;
                case 'A': // Upward arrow
                    loadPrevious(curLinePtr);
                    break;
                case 'B': // Downward arrow
                    loadNext(curLinePtr);
                    break;
                case 'C': // Right arrow
                    moveForward(curLinePtr);
                    break;
                case 'D': // left
                    moveBackward(curLinePtr);
                    break;
                default:
                    //printf("[");
                    processed = 0;
                    break;
            }
            if(escapeSequence == 2) escapeSequence = 0;
        
        }else if(escapeSequence == 1){
            if(ch == '['){
                escapeSequence = 2;
                processed = 1;
            }else{
                escapeSequence = 0;
            }
        }

        if(iscntrl(ch)){
            processed = 1;
            switch (ch)
            {
                case 3: // CTRL-C
                    state.length = 0;
                    state.curPos = 0;
                    state.pos = -1;
                    printf("^C\n\r<>");
                    break;
                case 4: // CTRL-D
                    control = 0;
                    break;
                case 13:
                    commit(curLinePtr);
                    printf("\n\r<>");
                    break;
                case 27: // ESC
                    escapeSequence = 1;
                    break;
                case 127: // backspace
                    backspace(curLinePtr);
                    break;
                default:
                    printf("(C:%d)",ch);
                    break;
            }
        }


        if(processed == 0){
            processed = 1;
            switch (ch)
            {
                case '\\': // Single char escape
                    escape = 0;
                    break;

                case '\'':
                    escape = '\'';
                case '\"':
                    escape = '"';
                default:
                    //printf("(%x)", ch& 0xFF);
                    addChar(curLinePtr, ch  );
                    //putchar(buffer[i]);
                    i++;
                    break;
            }
        }

    }

    printf("\n\r");
    system("clear");
    disableRawMode();
    
    free(state.content);
    if(state.draft) free(state.draft);
    if(state.history.length > 0){
        for(i = 0; i < state.history.length; i++){
            free(state.history.commands[i]);
        }
    }
    free(state.history.commands);
    #if DEBUG_ENABLED
        fclose(debugFile);
    #endif
    //system("stty echo");
    //system("stty cooked");
}