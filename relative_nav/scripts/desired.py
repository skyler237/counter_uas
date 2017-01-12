#!/usr/bin/env python

from __future__ import division
import rospy
from numpy import linspace
import numpy
import matplotlib.pyplot as plt
import time
from relative_nav.msg import DesiredState
from relative_nav.msg import Goal
from relative_nav.msg import FilterState
from random import randint
from collections import deque
import argparse
from random import random

node_id = -1
node_id_set = False


parser = argparse.ArgumentParser(description='Plot hex error (goal - current_state)')
parser.add_argument('X',type=float, help='X')
parser.add_argument('Y',type=float, help='Y')
parser.add_argument('Z',type=float, help='Z')
parser.add_argument('YAW',type=float, help='Yaw')
args = parser.parse_args()

def filterStateCallback(msg):
	global node_id
	global node_id_set
	node_id = msg.node_id
	node_id_set = True

node_name = 'desired_state_python_publisher'
current_topic = "current_state"
goal_topic = "desired_state"

rospy.init_node(node_name, anonymous=True)
#rospy.Subscriber(desired_topic, DesiredState, desiredStateCallback)
rospy.Subscriber(current_topic, FilterState,  filterStateCallback)
desired_state_pub = rospy.Publisher(goal_topic, DesiredState)
while not node_id_set:
	time.sleep(0.1)

print 'different', node_id
desired_state = DesiredState()
#print str(goal.node_id)
desired_state.node_id = node_id
desired_state.pose.x = args.X 
desired_state.pose.y = args.Y
desired_state.pose.z = args.Z
desired_state.pose.yaw = args.YAW

rate = rospy.Rate(1.0)
rate.sleep() #Needs to wait a little bit??
desired_state.pub.publish(desired_state)
print 'done'
