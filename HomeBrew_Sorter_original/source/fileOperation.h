#define MAX_HB 500
#define MAX_CAT 500

struct homebrew{
    char name[262];
    char path[262];
	char category[262];
    struct ScePspDateTime dateModify;
    char dateForSort[21];
	short int type;
};

struct categories{
	char name[262];
	char path[262];
	struct ScePspDateTime dateModify;
	char dateForSort[21];
	int repeated;//this stores if a category is repeated for merging it.
};

int getHBList(struct homebrew *HBlist, char *category, int flag);
int moveHBup(int index, struct homebrew *HBlist);
int moveHBdown(int index, struct homebrew *HBlist);
int saveHBlist(struct homebrew *HBlist, int HBcount);
int saveHBlistBM(struct homebrew *HBlist, int HBcount);//For browser mode, modifies eboot.pbp

int getCATList(struct categories *CAT);
int checkCATList(struct categories *CAT, struct categories *CAT_norep);
int moveCATup(int index, struct categories *CATlist);
int moveCATdown(int index, struct categories *CATlist);
int saveCATlist(struct categories *CATlist, int CATcount);
