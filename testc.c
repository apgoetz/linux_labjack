#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main()
{
  const int MESGSIZE = 14;
  char mesg[MESGSIZE];
  int temp;
  int result;
  int desc = open("/dev/lab0portC", O_RDONLY);
  result =  read(desc, mesg, sizeof(char)*MESGSIZE);
  if (result < 0 || result > MESGSIZE)
    {
      perror("Something messed up");
        close(desc);
      return -1;
    }
  printf("mesg was:\n %s\n", mesg);
  close(desc);
  
}
