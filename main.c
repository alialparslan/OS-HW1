#define _XOPEN_SOURCE // Compiler gives warning for wcwidth when this is not defined
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h> 
#include <unistd.h>  
#include <termios.h>
#include <wchar.h>
#include <locale.h> 
#include <sys/wait.h> 
#include <signal.h>
#include <errno.h>


#define CAPACIY_INCREMENT 10

#define DEBUG_ENABLED 0 // Enables or disables debug output globally

#define JUST_ECHO 0 // If it is 1 just echos back command

#define ADJUST_CAPACITY(array, count, elementSize) \
    if(count % CAPACIY_INCREMENT == 0 && count > 0) \
        args = realloc(array, elementSize * (count + CAPACIY_INCREMENT));

typedef struct{
    int line;
    int column;
}cursor_position;


typedef struct history_record{
    struct history_record *older;
    struct history_record *newer;
    char *command;
}history_record;


typedef struct{
    int terminalWidth; // How many columns terminal has
    //command_history history;
    history_record *history_last;
    history_record *history_pos; // 0 if current command is not loaded from history otherwise last loaded command's index in history
    char *content;
    int length; // length of current content in bytes
    int capacity; // Array capacity in bytes
    int curPos; // Points the character that cursor is currently over (First byte for multibytes) or length if it is at end of the line
    char* draft; // When surfing through history this stores draft command

    // curLine and curColumn needs valid terminalWidth and have to be recalculated upon change in terminal width.
    int curLine; // Line that where cursor currently at relative to start of command
    int curColumn; // Column that cursor currently on. 0 to terminalWidth-1
    int posSync; // Boolean that stores whether curLine and curColumn values are valid or not

    int width; // How much space in total characters use
    int lastCharStart; // Points to starting index of last char
    int expectedBytes; // How many bytes expected to complate multi byte char
    wchar_t wideChar; 
}shell_state;


void runCommand(char *command);
void addChar(shell_state *s, char ch);


#define HEXCHAR(char) char & 0xff

#define NEW_LINE(s) if(s->posSync && s->curColumn == 0) printf("<> "); else printf("\n\r<> ");

#define ESCAPES_BACKSPACE 0x8
#define ESCAPES_SINGLE 0x4
#define ESCAPES_DOUBLE 0x2
#define ESCAPES_CHECK_BACKSPACE(var) var & 0x8
#define ESCAPES_CLEAR_BACKSPACE(var) var = var & 0x7
#define ESCAPES_CHECK_SINGLE(var) var & 0x4
#define ESCAPES_CLEAR_SINGLE(var) var = var & 0xB
#define ESCAPES_CHECK_DOUBLE(var) var & 0x2
#define ESCAPES_CLEAR_DOUBLE(var) var = var & 0xD


#if DEBUG_ENABLED
    FILE* debugFile;
    void debugDumpState(shell_state* s){
        int i = 0; 
        fprintf(debugFile, "length: %d, curPos: %d, capacity: %d", s->length, s->curPos, s->capacity); 
        fprintf(debugFile, ",terminalWidth: %d, curLine: %d, curColumn: %d", s->terminalWidth, s->curLine, s->curColumn);
        if(s->draft){ 
            fprintf(debugFile, "\nDraft: "); 
            while(s->draft[i]){ 
                if(i > 0) fprintf(debugFile, ","); 
                fprintf(debugFile, "%X", s->draft[i] & 0xff); 
                i++; 
            } 
        }else fprintf(debugFile, ", draft: 0"); 
        fprintf(debugFile, "\nContent: "); 
        for(i = 0; i < s->length; i++){ 
            if(i > 0) fprintf(debugFile, ","); 
            fprintf(debugFile, "%X", s->content[i] & 0xff); 
            if((s->content[i] & 0xC0) == 0x80) fprintf(debugFile, "*");
        } 
        fprintf(debugFile, "\n\n");
        fflush(debugFile);
    }
    #define DEBUG( ...) fprintf (debugFile, __VA_ARGS__)
    #define DEBUG_DUMP_STATE(l, label) \
        fprintf(debugFile, "State (%s):\n", label); \
        debugDumpState(l);
#else
    #define DEBUG
    #define DEBUG_DUMP_STATE
#endif


struct termios termios_config;
int resizeOccured = 0;

void getCursorPosition(cursor_position *cp){
    char ch;
    cp->line=0;
    cp->column=0;
    printf("\e[6n");
    while(getchar() != '\e'); // One shouldn't do this
    getchar();
    while((ch = getchar()) != ';') cp->line = (ch-48) + cp->line*10;
    while((ch = getchar()) != 'R') cp->column = (ch-48) + cp->column*10;
}

char* makeStr(char* string, int length){
    DEBUG("makestr length:%d \n", length);
    char* new = malloc(sizeof(char) * (length+1));
    memcpy(new, string, length);
    string[length] = '\0';
    return new;
}

void dumbPrint(char* string){
    while(*string){
        putchar(*string);
        if(*string == '\n') printf("\r");
        string++;
    }
}

void reloadTerminalWidth(shell_state *s){
    cursor_position cp;
    int saveColumn;
    getCursorPosition(&cp);
    saveColumn = cp.column;
    printf("\e[999C");
    getCursorPosition(&cp);
    s->terminalWidth = cp.column;
    printf("\e[%d;%dH", cp.line, saveColumn);
}

//Returns how much space given character takes in terminal
int getCharWidthAndSkip(char *string, int *i){
    wchar_t wc;
    char ch = *string;
    int width = 0;
    if(ch & 0x80){ //Part of multi byte character
        if(ch & 0x40){
            //First byte of multi byte character
            if(ch & 0x20){
                if(ch & 0x10){ // 4 btye
                    *i += 4;
                    wc = ((ch & 0x0F)<<18) + (( *(string+1) & 0x3F)<<12) + ((*(string+2) & 0x3F)<<6) + (*(string+3) & 0x3F);
                }else{ // 3 byte
                    *i += 3;
                    wc = ((ch & 0x0F)<<12) + (( *(string+1) & 0x3F)<<6) + ((*(string+2) & 0x3F));
                }
            }else{
                // 2 byte
                *i += 2;
                wc = ((ch & 0x1F)<<6) + (*(string+1) & 0x3F);
            }
            width = wcwidth(wc);
        }else{
            // This normall should never happen
            *i += 1;
        }
    }else{
        *i += 1;
        width = wcwidth(ch);
    }
    if(width < 0) return 0;
    return width;
}

// Until we are able to maintain valid cursor position after any operation this is needed.
void updateCursorPos(shell_state *s){
    int line = 0; // Current line relative to beginning
    int column = 3;
    int i = 0;
    char ch;
    wchar_t wc;
    int width; //Width of char
    while(i < s->curPos){
        ch = s->content[i++];
        width = 0; //getCharWidthAndSkip(s->content+i, &i);
        if(ch & 0x80){ //Part of multi byte character
            if(ch & 0x40){
                //First byte of multi byte character
                if(ch & 0x20){
                    if(ch & 0x10){ // 4 btye
                        wc = ((ch & 0x0F)<<18) + ((s->content[i++] & 0x3F)<<12) + ((s->content[i++] & 0x3F)<<6) + (s->content[i++] & 0x3F);
                    }else{ // 3 byte
                        wc = ((ch & 0x0F)<<12) + ((s->content[i++] & 0x3F)<<6) + (s->content[i++] & 0x3F);
                    }
                }else{
                    // 2 byte
                    wc = ((ch & 0x1F)<<6) + (s->content[i++] & 0x3F);
                }
                width = wcwidth(wc);
                DEBUG("wc:%d, width:%d\n", wc, width);
                if(width < 0) width = 0;
            }else{
                // Continuation byte
                DEBUG("This should never happend 11!\n");
            }
        }else{
            width = wcwidth(ch);
        }
        column += width;
        if(column > s->terminalWidth){
            line++;
            if(column > s->terminalWidth) column = width;
            else column = 0;
        }else if(column == s->terminalWidth){
            line++;
            column = 0;
        }
        if(ch == '\n'){
            line++;
            column = 2;
        }
    }
    s->curLine = line;
    s->curColumn = column;
    //DEBUG_DUMP_STATE(s, "updateCursorPos_end");
    s->posSync = 1;
}

//Just clears terminal does not change command in memory. printLine should be called after changes made in command
void clearLine(shell_state *s){
    // If curPos smaller then terminalWidth we are in first line for sure, so no need to go up or any calculation for that
    if(s->curPos+6 > s->terminalWidth){ 
        if(!s->posSync) updateCursorPos(s);
        if(s->curLine > 0) printf("\e[%dA", s->curLine);  
    }else{
        DEBUG("clearLine_else: curPos < terminalWidth\n");
    }
    printf("\e[999D\e[J<> ");
}

//Assumes clearLine run before this
void printLine(shell_state *s){
    int i;
    int l;
    int width;
    s->curLine = 0;
    s->curColumn = 3;
    if(s->length > 0){
        for(i = 0; i<s->curPos;){ // Since we need column and line until curPos, this loop goes until curPos
            l = i;
            width = getCharWidthAndSkip(s->content+i, &i);
            if(s->content[l] == '\n'){
                s->curLine++;
                s->curColumn = 2;
                printf("\n\r> ");
            }else{
                while(l < i) putchar(s->content[l++]);
            }
            
            
            s->curColumn += width;
            if(s->curColumn >= s->terminalWidth){
                if(s->curColumn == s->terminalWidth){
                    s->curColumn = 0;
                    s->curLine++;
                    printf("a\e[D\e[K");
                }else{
                    s->curColumn = width;
                    s->curLine++;
                }
                
            }
        }

        if(i < s->length){ // If there is string after curPos
            printf("\e7");
            for(; i<s->length;i++) putchar(s->content[i]);
            if(s->curPos != s->length) printf("\e8");
        }

    }
    s->posSync = 1;
}

// Maybe proper way is printing whole screen from beginning.
// curPos has to point first byte of multi byte characters in order to this func work properly.
void reloadLine(shell_state *s){
    s->posSync = 0;
    clearLine(s);
    printLine(s);
}



static inline void stateInit(shell_state *s){
    s->content = malloc(sizeof(char) * CAPACIY_INCREMENT);
    s->length = 0;
    s->capacity = CAPACIY_INCREMENT;
    s->curPos = 0;
    s->history_pos = NULL;
    s->history_last = NULL;
    s->draft = 0;
    s->width = 0;
    s->expectedBytes = 0;
    s->lastCharStart = 0;

    s->curColumn = 3;
    s->curLine =0;
    s->posSync = 0;
}


void historyAdd(shell_state *s, char *command){
    history_record *record = malloc(sizeof(history_record));

    if(s->history_last) s->history_last->newer = record;
    record->command = command;
    record->older = s->history_last;
    record->newer = NULL;
    s->history_last = record;
}

void loadFromHistory(shell_state *s, history_record *record){
    int i = 0;

    if(s->length > 0){
        if(s->history_pos == NULL){
            s->draft = makeStr(s->content, s->length);
            DEBUG("loadFromHistory: Content saved as draft.\n");    
        }
        clearLine(s);
    }
    s->history_pos = record;
    s->curColumn = 3;
    s->curLine = 0;
    s->length = 0;
    s->curPos = 0;
    while(record->command[i]){
        addChar(s, record->command[i]);
        i++;
    }
    s->posSync = 0;
    DEBUG_DUMP_STATE(s, "loadFromHistory_end");
    /*s->curPos = s->length = printf("%s", record->command);

    if(s->curPos > s->capacity){
        free(s->content);
        s->content = malloc(sizeof(char)*s->length);
    }
    memcpy(s->content, record->command, s->length);
    
    s->posSync = 0;*/
}

void loadPrevious(shell_state *s){
    if(s->history_pos){
        if(s->history_pos->older) loadFromHistory(s, s->history_pos->older);
    }else if(s->history_last) loadFromHistory(s, s->history_last);
}


void loadNext(shell_state *s){
    if(s->history_pos == NULL) return;
    if(s->history_pos->newer){
        loadFromHistory(s, s->history_pos->newer);
    }else{
        // Load draft if there is one
        if(s->length > 0) clearLine(s);
        if(s->draft){
            DEBUG("loadNext: Loading the draft.\n");
            if(s->content) free(s->content);
            s->content = s->draft;
            s->draft = 0;
            s->curPos = printf("%s", s->content);
            s->capacity = s->curPos;
            s->length = s->curPos;
        }else{
            s->length = 0;
            s->curPos = 0;
        }
        s->history_pos = NULL;
        s->posSync = 0;
    }
}

//adds a char that starts at where curPos points
void addChar(shell_state *s, char ch){
    wchar_t wideChar;
    int i;
    int l;
    int width = 0;
    int continuationByte = 0;
    if(s->length+4 > s->capacity){ //We may open space in the middle of string for multi byte sequence therefore length+3
        s->capacity += CAPACIY_INCREMENT;
        s->content = realloc(s->content, sizeof(char) * s->capacity );
    }

    if(ch & 0x80){
        if(ch & 0x40){
            #if DEBUG_ENABLED
                if(s->expectedBytes > 0) DEBUG("This should never ever happen! addChar: expected char didn't received!\n");
            #endif
            //First byte of multi byte character
            s->lastCharStart = s->curPos;
            if(ch & 0x20){
                if(ch & 0x10){ // 4 btye
                    s->expectedBytes = 3;
                    s->wideChar = (ch & 0x0F)<<18;
                }else{ // 3 byte
                    s->expectedBytes = 2;
                    s->wideChar = (ch & 0x0F)<<12;
                }
            }else{
                // 2 byte
                s->expectedBytes = 1;
                s->wideChar = (ch & 0x1F)<<6;
            }
        }else{
            // Continuation byte
            continuationByte = 1;
            s->wideChar += (ch & 0x3F)<<(--s->expectedBytes*6);
            if(s->expectedBytes == 0){ //Multi byte sequence completed!
                width = wcwidth(s->wideChar);
            }
        }
    }else{ //Single byte
        #if DEBUG_ENABLED
            if(s->expectedBytes > 0) DEBUG("This should never ever happen! addChar: expected char didn't received!\n");
        #endif
        width = wcwidth(ch);
    }

    //if(!s->posSync) updateCursorPos(s);
    DEBUG_DUMP_STATE(s, "addChar before if");
    if(s->curPos == s->length){
        s->content[s->curPos] = ch;
        s->curPos++;
        s->length++;
        putchar(ch);

        if(width > 0){
            s->curColumn += width;
            if(s->curColumn == s->terminalWidth){
                s->curColumn = 0;
                s->curLine++;
                printf("a\e[D\e[K");
            }else if(s->curColumn > s->terminalWidth){
                s->curColumn = width;
                s->curLine++;
            }
        }else if(ch == '\n'){
            s->curLine++;
            s->curColumn = 2;
            printf("\r> ");
        }
    }else{
        //DEBUG("addChar curPos != length");
        if(continuationByte){
            s->content[s->curPos] = ch;
            s->curPos++;
            s->length++;
            if(s->expectedBytes == 0){
                printLine(s);
            }
        }else{
            clearLine(s);
            l = 1 + s->expectedBytes;
            for(i=s->length-1; i >= s->curPos; i--){
                s->content[i+l] = s->content[i];
            }
            s->content[s->curPos] = ch;
            s->curPos++;
            s->length++;
            if(s->expectedBytes == 0){
                printLine(s);
            }
        }
    }



}

// When curser is over a multibyte utf8 char curPos will point first char of that
// So regardless of length of char first we decrease curPos by one
// Then we check if we have stepped over a multibyte char and decrease until curPos points first byte of character
void moveBackward(shell_state *s){
    //updateCursorPos(s);
    if(s->curPos > 0){
        clearLine(s);
        while((s->content[--s->curPos] & 0xC0) == 0x80);
        printLine(s);
    }
}

void moveForward(shell_state *s){
    //updateCursorPos(s);
    if(s->curPos < s->length){
        clearLine(s);
        s->curPos++;
        while((s->content[s->curPos] & 0xC0) == 0x80) s->curPos++;
        printLine(s);
    }
}


void backspace(shell_state *s){
    int i, j, x, moveBack = 0;
    if(s->curPos > 0){
        clearLine(s);
        //write(STDOUT_FILENO, "\033[1D\x1b[K\e7", 9);
        x = 1;
        i = s->length - s->curPos;
        s->curPos--;
        while((s->content[s->curPos] & 0xC0) == 0x80){
            s->curPos--;
            x++;
        }
        s->length -= x;
        
        for(j = 0; j < i ; j++){
            DEBUG("Copying: %X from %d to %d\n", HEXCHAR(s->content[s->curPos+j+x]), (s->curPos)+j+x, (s->curPos)+j);
            s->content[s->curPos+j] = s->content[s->curPos+j+x];
        }
        DEBUG("moving back %d\n", moveBack);
        //if(moveBack>0) printf("\033[%dD", moveBack);
        DEBUG_DUMP_STATE(s,"Backspace end");
        printLine(s);
    }
}

void delete(shell_state *s){
    int i, j, x, moveBack = 0;
    int width;
    if(s->curPos < s->length){
        clearLine(s);
        //write(STDOUT_FILENO, "\x1b[K", 3);
        x = 0;
        width = getCharWidthAndSkip(s->content+s->curPos, &x);
        //while((s->content[s->curPos+x] & 0xC0) == 0x80) x++;
        
        i = s->length - s->curPos;
        for(j = 0; j < i ; j++){
            s->content[s->curPos+j] = s->content[s->curPos+j+x];
        }
        //if(moveBack>2) printf("\033[%dD", moveBack-2);
        s->length -= x;
        printLine(s);
    }
}

void commit(shell_state *s){
    if(s->length > 0){
        if(s->curPos < s->length){
            clearLine(s);
            s->curPos = s->length;
            printLine(s);
        }
        printf("\n\r");
        if(s->capacity == s->length || s->capacity/s->length > 1.04){
            s->content = realloc(s->content, sizeof(char) * (s->length+1));
        }
        s->content[s->length] = '\0';
        historyAdd(s, s->content);
        runCommand(s->content);
        s->content = malloc(sizeof(char) * CAPACIY_INCREMENT);
        s->length = 0;
        s->capacity = CAPACIY_INCREMENT;
        s->curPos = 0;
        s->curLine = 0;
        s->curColumn = 3;
    }else{
        s->curPos = 0;
    }
    s->history_pos = 0;
}

// Allocates new command string to command variable and returns array of pointers that points to locations in that string.
char** parseCommand(char **command){
    char ch; //temp to store character that currently being processsed.
    int i = 0; // Position in old command string
    int newI = 0; // Position in new command string
    int process; // Stores whether character processed or not 
    int argStart = -1; // Where the latest argument starts at new string
    int argCount = 0;
    int argLength = 0;
    char *oldStr = *command;
    char escapes = 0;
    char *newStr = malloc(sizeof(char)*(CAPACIY_INCREMENT + 3));
    int args_capacity = CAPACIY_INCREMENT;
    char** args = malloc(sizeof(char*)*(CAPACIY_INCREMENT+1));
    while(oldStr[i]){
        ch = oldStr[i];
        ADJUST_CAPACITY(args, argCount+1, sizeof(char*));
        ADJUST_CAPACITY(newStr, newI+3, sizeof(char*));

        if(escapes){
            process = 0;
            if(escapes & 0x8){
                escapes = escapes ^ 0x8;
                process = 1;
            }else if(escapes & 0x4){
                if(ch == '\'') escapes = 0;
                else process = 1;
            }else{ 
                if(ch == '\"') escapes = 0;
                else process = 1;
            }
            if(process){
                if(argLength == 0) args[argCount++] = newStr+newI; 
                newStr[newI++] = ch;
                argLength++;
            }
            
        }else{
            switch (ch)
            {
            case '|':
                if(argLength > 0){
                    newStr[newI++] = '\0';
                    argLength = 0;
                }
                args[argCount++] = newStr+newI;
                newStr[newI++] = '|';
                newStr[newI++] = '\0';
                break;
            case ' ':
                if(argLength > 0){
                    newStr[newI++] = '\0';
                    argLength = 0;
                }
                break;
            case '\\':
                escapes = 0x8;
                break;
            case '\'':
                escapes = 0x4;
                break;
            case '\"':
                escapes = 0x2;
                break;
            default:
                if(argLength == 0) args[argCount++] = newStr+newI;
                newStr[newI++] = ch;
                argLength++;
                break;
            }


        }
        i++;
    }
    if(argLength > 0){
        newStr[newI] = '\0';
    }
    i = 0;
    while(args[i]) printf("%s\n\r", args[i++]);
    *command = newStr;
    args[argCount] = 0;
    return args;
}

// Executes command and returns a pipe
int* executeCommand(char **args, int *inputPipe){
    int execResult;
    int pid;
    int pipeOperator = 0;
    int *outputPipe = malloc(sizeof(int)*2);
    if (pipe(outputPipe)==-1) printf("Pipe Error!\n\r");

    while(args[pipeOperator] && !(args[pipeOperator][0] == '|' && args[pipeOperator][1] == '\0' ) ) pipeOperator++;
    if(args[pipeOperator] == NULL)
        pipeOperator = 0;

    pid = fork();
    if(pid == -1){
        printf("Fork Error!\n\r");
        free(outputPipe);
        return NULL;
    }else if(pid != 0){
        //parent
        if(pipeOperator)
            return executeCommand(args+pipeOperator+1, outputPipe);
        else
            return outputPipe;
            
    }
    //child
    //Check if there is a pipe operator
    if(pipeOperator) args[pipeOperator] = NULL;
    if(inputPipe){
        wait(NULL);
        close(inputPipe[1]);
        dup2(inputPipe[0], STDIN_FILENO);
    }
    dup2(outputPipe[1], STDOUT_FILENO);
    dup2(outputPipe[1], STDERR_FILENO);
    execResult = execvp(args[0], args);
    if(execResult < 0) printf("Error: %s!", strerror(errno));
    close(outputPipe[1]);
    exit(1);
}

void runCommand(char *command){
    pid_t pid;
    int *outputPipe; // 0>Reading 1>Writing
    char *output;
    int result, i;
    char *tempCommandStr = command;
    char **args = parseCommand(&tempCommandStr);
    #if JUST_ECHO
        //printf("\n\r");
        dumbPrint(command);
        return;
    #endif

    if(strcmp(args[0], "cd") == 0){
        if(args[1] == NULL) return;
        if(chdir(args[1]) < 0){
            printf("Error: %s!", strerror(errno));
        }
        return;
    }

    outputPipe = executeCommand(args, NULL);
    if(outputPipe == NULL){
        printf("Execution Error!\n\r");
        return;
    }

    wait(NULL);
    close(outputPipe[1]);
    output = malloc(sizeof(char)*CAPACIY_INCREMENT);
    i = 0;
    while( ( result = read(outputPipe[0], output+i, CAPACIY_INCREMENT) ) > 0){
        i += result;
        output = realloc(output, sizeof(char) * (i+CAPACIY_INCREMENT));
    }
    output[i] = '\0';
    i = 0;
    //printf("\n\r");
    while(output[i]){
        putchar(output[i]);
        if(output[i] == '\n' && output[i+1] != '\r') putchar('\r');
        i++;
    }
    free(tempCommandStr);
    free(args);
    free(outputPipe);
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
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // Disabling all those above.
    // ICRNL: Translate carriage return to newline on input
    // IXON: CTRL-S and CTRL-Q
    raw.c_iflag &= ~(ICRNL | IXON); // Disable those
    raw.c_oflag &= ~(OPOST);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void resizeEventHandler(int signal){
    resizeOccured = 1;
}

int main(int argc, char *argv[]){
    struct sigaction sa;
    char** history;
    int historyCapacity;
    int historyI = 0;
    char buffer[150];
    int i = 0;
    int quote = 0;
    char escapes = 0;
    int escapeSequence = 0;
    char ch;
    int control = 1;
    int processed = 0;
    setlocale (LC_ALL,"");
    #if DEBUG_ENABLED
        debugFile = fopen("debug.txt", "w+");
    #endif
    shell_state state;
    shell_state *statePtr = &state;
    stateInit(statePtr);

    //system("stty raw");
    //system("stty -echo");
    enableRawMode();
    system("clear");
    write(STDOUT_FILENO, "\x1b[2J", 4);
    reloadTerminalWidth(statePtr);
    printf("Welcome To AlpCLI - Press CTRL-D to quit");
    NEW_LINE(statePtr);

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = resizeEventHandler;
    if(sigaction(SIGWINCH, &sa, NULL) == -1) printf("Error when setting up SIGWINCH signal handler!\n\r");

    while(control){
        ch = getchar();
        if(ch == EOF) continue; // Signal handler prints EOF to stdin.
        processed = 0;

        if(escapeSequence == 3){
            if(ch == '~'){
                delete(statePtr);
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
                    loadPrevious(statePtr);
                    break;
                case 'B': // Downward arrow
                    loadNext(statePtr);
                    break;
                case 'C': // Right arrow
                    moveForward(statePtr);
                    break;
                case 'D': // left
                    moveBackward(statePtr);
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


        if(processed == 0 && iscntrl(ch)){
            processed = 1;
            switch (ch)
            {
                case 3: // CTRL-C
                    state.length = 0;
                    state.curPos = 0;
                    state.curLine = 0;
                    state.curColumn = 3;
                    state.history_pos = NULL;
                    printf("^C");
                    NEW_LINE(statePtr);
                    break;
                case 4: // CTRL-D
                    control = 0;
                    break;
                case '\r': // Enter
                    if(escapes){
                        addChar(statePtr, '\n');
                    }else{
                        commit(statePtr);
                        NEW_LINE(statePtr);
                    }
                    break;
                case 27: // ESC
                    escapeSequence = 1;
                    break;
                case 127: // backspace
                    backspace(statePtr);
                    break;
                default:
                    //printf("(C:%d)",ch);
                    break;
            }
        }


        if(processed == 0){
            processed = 1;
            switch (ch)
            {
                case '\\':
                    escapes = escapes ^ 0x8;
                case '\'':
                    if(escapes == 0x4 || !escapes) escapes = escapes ^ 0x4;
                case '\"':
                    if(escapes == 0x2 || !escapes) escapes = escapes ^ 0x2;
                default:
                    //printf("(%x)", ch& 0xFF);
                    addChar(statePtr, ch);
                    break;
            }
        }
        if(ch != '\\') ESCAPES_CLEAR_BACKSPACE(escapes);
        

        //We need to do this here t avoid inturrupting multibyte char sequences
        if(resizeOccured && !state.expectedBytes){
            reloadTerminalWidth(statePtr);
            //reloadLine(statePtr);
            resizeOccured = 0;
        }
    }

    printf("\n\r");
    system("clear");
    disableRawMode();
    
    free(state.content);
    if(state.draft) free(state.draft);
    while(state.history_last){
        state.history_pos = state.history_last;
        state.history_last = state.history_last->older;
        free(state.history_pos->command);
        free(state.history_pos);
    }
    #if DEBUG_ENABLED
        fclose(debugFile);
    #endif
    //system("stty echo");
    //system("stty cooked");
}