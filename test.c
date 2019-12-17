#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>


/*
 * Bir stringte bulunan bosluk ve yenisatir(\n) karakterlerini basindan ve sonundan siler
 * */
void boslukSil(char* str);


/*
 * Kullanici tarafindan girilen komutta |(pipe) varsa ona gore ayiran fonksiyon.
 * String dizisi ve kac kelimeden olustugu dondurulur
 * */
void ayirma(char** param,int *nr,char *buf,const char *c);


/*
Basit bir komutu calistiran fonksiyon.
 str - kullanici tarafindan girilen komut
*/
void basitKomut(char** str);


/*
 * Pipelanmis birkac komutu calistiran fonksiyon.
 * nr - komut sayisini tutan degisken
 * buf - komutlar
 * */
void pipeliKomut(char **buf, int nr);


int main()
{
    char komut[500];
    char *buffer[100];
    char *param[100];
    char dr[1024]; // Directory'i tutan degisken
    int nr=0;

    while(1){
        //Bulundugumuz Direktoriyi yazdirir
        if (getcwd(dr, sizeof(dr)) != NULL)
            printf( "%s  ", dr);
        else 	perror("Olunmasi gereken direktory bulunamadi\n");

        //Buffer overflow engellemek icin
        fgets(komut, 500, stdin);

        // '|' isareti varmi diye kontrol edilir. Varsa birden fazla komut vardir ve birbirinden ayrilmalidir
        if(strchr(komut,'|')){// Ayirma islemi
            ayirma(buffer,&nr,komut,"|");
            pipeliKomut(buffer, nr);
        }

        else{//Tek komut calistirma kismi
            ayirma(param,&nr,komut," ");
            if(strstr(param[0],"cd")){//cd komutu yazildiysa direktoriyi duzenle
                chdir(param[1]);
            }
            else if(strstr(param[0],"exit")){
                exit(0);
            }
            else basitKomut(param);
        }
    }
    return 0;
}


void boslukSil(char* str){
    if(str[strlen(str)-1]==' ' || str[strlen(str)-1]=='\n')
        str[strlen(str)-1]='\0';
    if(str[0]==' ' || str[0]=='\n') memmove(str, str+1, strlen(str));
}

void ayirma(char** param,int *nr,char *buf,const char *c){
    char *token;
    token=strtok(buf,c);
    int pc=-1;
    while(token){
        param[++pc]=malloc(sizeof(token)+1);
        strcpy(param[pc],token);
        boslukSil(param[pc]);
        token=strtok(NULL,c);
    }
    param[++pc]=NULL;
    *nr=pc;
}

void basitKomut(char** str){
    if(fork()>0){
        //parent
        wait(NULL);
    }
    else{
        //child
        execvp(str[0],str);
        perror(  "hatali giris"   "\n");
        exit(1);
    }
}

void pipeliKomut(char **buf, int nr){
    if(nr>10) { // 10 dan fazla komut varsa calistirma
        return;
    }

    int fd[10][2];
    int i;
    int pc;
    char *arg[100];

    for(i=0;i<nr;i++){
        ayirma(arg,&pc,buf[i]," ");
        if(i!=nr-1){
            if(pipe(fd[i])<0){
                perror("pipelama islemi basarisiz\n");
                return;
            }
        }
        if(fork()==0){//child
            if(i!=nr-1){
                dup2(fd[i][1],1);
                close(fd[i][0]);
                close(fd[i][1]);
            }

            if(i!=0){
                dup2(fd[i-1][0],0);
                close(fd[i-1][1]);
                close(fd[i-1][0]);
            }
            execvp(arg[0],arg);
            perror("hatali giris");
            exit(1);
        }
        //parent
        if(i!=0){//ikinci process
            close(fd[i-1][0]);
            close(fd[i-1][1]);
        }
        wait(NULL);
    }
}