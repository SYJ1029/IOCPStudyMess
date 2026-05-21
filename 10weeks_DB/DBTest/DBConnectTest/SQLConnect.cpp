// SQLBindCol_ref.cpp  
// compile with: odbc32.lib  
#include <windows.h>  
#include <stdio.h>  

#define UNICODE  
#include <sqlext.h>  

#define NAME_LEN 20  
#define PHONE_LEN 60

void show_error() {
    printf("error\n");
}

int main() {
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt = 0;
    SQLRETURN retcode;
	SQLINTEGER user_id, user_level;
    SQLWCHAR user_name[NAME_LEN];
    SQLLEN cbName = 0, cbCustID = 0, cbPhone = 0;

    // Allocate environment handle  
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

    // Set the ODBC version environment attribute  
    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
        retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

        // Allocate connection handle  
        if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
            retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

            // Set login timeout to 5 seconds  
            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

                // Connect to data source  
                retcode = SQLConnect(hdbc, (SQLWCHAR*)L"2026_Game_Test", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

                // Allocate statement handle  
                if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                    retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

                    // retcode = SQLExecDirect(hstmt, (SQLWCHAR*)L"SELECT CustomerID, ContactName, Phone FROM CUSTOMERS ORDER BY 2, 1, 3", SQL_NTS);
                    // retcode = SQLExecDirect(hstmt, (SQLWCHAR*)L"SELECT user_id, user_name, user_level FROM dbo.user_table ORDER BY 1", SQL_NTS);
                    // 10레벨 초과인 유저를 가져오기 위한 저장 프로시저 실행
                    retcode = SQLExecDirect(
                        hstmt, 
                        (SQLWCHAR*)L"EXEC select_highlevel @Param = 10", 
                        SQL_NTS
                    );
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {

                        // Bind columns 1, 2, and 3  
                        retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &user_id, 10, &cbCustID);
                        retcode = SQLBindCol(hstmt, 2, SQL_C_WCHAR, user_name, NAME_LEN, &cbName);
                        retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &user_level, 10, &cbPhone);

                        // Fetch and print each row of data. On an error, display a message and exit.  
                        for (int i = 0; ; i++) {
                            retcode = SQLFetch(hstmt);
                            if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
                                show_error();
                            if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
                            {
                                //replace wprintf with printf
                                //%S with %ls
                                //warning C4477: 'wprintf' : format string '%S' requires an argument of type 'char *'
                                //but variadic argument 2 has type 'SQLWCHAR *'
                                //wprintf(L"%d: %S %S %S\n", i + 1, sCustID, szName, szPhone);  
                                printf("%d: %d %ls %d\n", i + 1, user_id, user_name, user_level);
                            }
                            else
                                break;
                        }
                    }

                    // Process data  
                    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
                        SQLCancel(hstmt);
                        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
                    }

                    SQLDisconnect(hdbc);
                }

                SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
}