/****************************************************************************
*  model config header file
****************************************************************************/
#ifndef _MODEL_CONFIG_H_
#define _MODEL_CONFIG_H_

#include <iostream>
#include <vector>



// 1 class, person
#define CLASS_NUM           1

#define NUM_POINTS          17


#define LETTERBOX_ROWS      640
#define LETTERBOX_COLS      640

#define SCORE_THRESHOLD     0.4f
#define NMS_THRESHOLD       0.45f

const std::vector<std::string> g_classes_name{
    "person"
};



#endif
