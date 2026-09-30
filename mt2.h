

#ifndef MT2_H
#define MT2_H

#ifdef __cplusplus
extern "C" {
#endif


const char* mt2_version(void);


int mt2_has_zip_support(void);


int mt2_exec(int argc, const char** argv);


int mt2_exec_str(const char* cmd_str);


void mt2_set_language(int lang);


const char* mt2_help(void);


#ifdef __cplusplus
}
#endif

#endif
