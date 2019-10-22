# dbd-modules
SQL logging and document root lookup
## mod_log_dbd
**mod_log_dbd** re-directs the output from a **[CustomLog](http://httpd.apache.org/docs/current/mod/mod_log_config.html#customlog)** directive to a database table.

## mod_vhost_dbd
**mod_vhost_dbd** overrides the [DocumentRoot](http://httpd.apache.org/docs/current/mod/core.html#documentroot) directory using an SQL query. 

# Instructions
# Building mod_vhost_dbd and mod_log_dbd

## Windows

#### Use the Visual C++ NMAKE utility to compile, link, and install the modules
    
    NMAKE -f Makefile.win APACHE=apachedir
    NMAKE -f Makefile.win APACHE=apachedir install

where `apachedir` is a top level Apache 2.2+ directory. For example: 
    
    NMAKE -f Makefile.win APACHE=C:\Apache2
    NMAKE -f Makefile.win APACHE=C:\Apache2 install

> ##### Note for Apache version 2.2.6 or earlier
> 
> Apache 2.2 prior to version 2.2.7 is missing several header files in the /include directory on Windows. See [Apache Bug 43715](http://issues.apache.org/bugzilla/show_bug.cgi?id=43715). If you are using Apache 2.2.0 through 2.2.6, copy these two header files from the Apache source directory (or the [Apache SVN repository](http://svn.apache.org/viewvc/httpd/httpd/tags/)) to the Apache \include directory: 
> 
>   * modules\database\mod_dbd.h 
>   * modules\loggers\mod_log_config.h 

## Unix

#### User the Apache apxs utility to compile, link, and install the modules
    
    apxs -c mod_vhost_dbd.c
    apxs -i mod_vhost_dbd.la
    apxs -c mod_log_dbd.c
    apxs -i mod_log_dbd.la

## [Configuring and using mod_log_dbd](mod_log_dbd)
## [Configuring and using mod_vhost_dbd](mod_vhost_dbd)
