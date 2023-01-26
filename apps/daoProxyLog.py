#!/usr/bin/env python3
'''
Script used to simulate the PWFS image from the commands applied to the DM
'''

import daoProxy 
###########
# We defined 5558(sub) and 5559(pub) the port for the log message
if __name__ == '__main__':
    daoProxy.daoProxy(5558, 5559)        

