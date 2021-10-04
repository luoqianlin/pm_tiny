//
// Created by qianlinluo@foxmail.com on 2022/7/27.
//

#include <stdio.h>
#include <unistd.h>
#include <string>
#include <iostream>
int main(int argc,char*argv[]){
    std::string x= getenv("PM_TINY_HOME");
    std::cout<<"`"<<x<<"`"<<std::endl;
//    for(int i=0;i<5;i++){
//        printf("%s ===%d===\n",argv[0],i);
//        sleep(1);
//    }
    return -1;
}