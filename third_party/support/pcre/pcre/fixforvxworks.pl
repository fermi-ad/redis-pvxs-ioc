#!/usr/bin/env perl

# check if pcre_internal.h has already been fixed
open (IN, "pcre_internal.h")
    or die "pcre_internal.h not found";

while (<IN>) {
    exit if $_ =~ /#ifdef vxWorks/;
}

# fix the file
seek (IN, 0, SEEK_SET);

open (OUT, '>', "pcre_internal.h.new")
    or die "cannot write fixed version of pcre_internal.h";

while (<IN>) {
    last if $_ =~ /typedef int BOOL;/;
    print OUT $_;
}

print OUT "#ifdef vxWorks\n";
print OUT "#undef OK\n";
print OUT "#else\n";
print OUT "typedef int BOOL;\n";
print OUT "#endif\n";
while (<IN>) {
    print OUT $_;
}

close OUT
    or die "cannot close version of pcre_internal.h";
close IN;

unlink ("pcre_internal.h")
    or die "cannot remove old version of pcre_internal.h";
rename ("pcre_internal.h.new", "pcre_internal.h")
    or die "cannot replace pcre_internal.h with fixed version";

