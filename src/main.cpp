extern bool AppBootstrap(int argc, char const *argv[]);
extern void AppRun();
extern void AppShutdown();

int main(int argc, char const *argv[])
{
    if (AppBootstrap(argc, argv))
    {
        AppRun();
        AppShutdown();
    }
    return 0;
}
