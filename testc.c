#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
int main()
{
  
  int temp;
  int desc = open("/dev/lab0portC", O_RDONLY);
  read(desc, &temp, sizeof(int));
  close(desc);
  
}
