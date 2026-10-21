#pragma once
class OBSBasic {
public:
	static OBSBasic *Get()
	{
		static OBSBasic instance;
		return &instance;
	}
	void SaveProject() { ++saves; }
	int saves = 0;
};
