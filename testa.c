#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main()
{
  
  char temp;
  char ttemp = 7;
  int desc = open("/dev/lab0portA", O_RDWR);
  sleep(3);
  write(desc, &ttemp, sizeof(char));
  read(desc, &temp, sizeof(char));
  printf("time since toggled: %d\n", temp);
  close(desc);

}
