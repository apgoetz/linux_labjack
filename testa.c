#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main()
{
  
  int temp;
  int desc = open("/dev/lab0portA", O_RDONLY);
  read(desc, &temp, sizeof(int));
  sleep(10);
  close(desc);

}
