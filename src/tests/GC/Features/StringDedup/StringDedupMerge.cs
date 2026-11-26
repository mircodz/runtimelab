// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// With DOTNET_GCStringDedupMerge=1 the GC looks for duplicate strings among the ones that leave gen1, and the GC that compacts replaces
// every reference to a duplicate by one to the first string with that content.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Xunit;

public class StringDedupMerge
{
    private const int Contents = 1000;
    private const int Copies = 100;

    [Fact]
    public static void TestEntryPoint()
    {
        string[] plain = CreateStrings(Contents, Copies, "plain ");

        // The identity of a string that has a hash code is observable, so it is not merged. These strings have the content of the first
        // string of 'plain', the one that the others are merged into.
        string[] hashCoded = CreateStrings(1, Copies, "plain ");
        int[] hashCodes = new int[Copies];
        for (int i = 0; i < Copies; i++)
        {
            hashCodes[i] = RuntimeHelpers.GetHashCode(hashCoded[i]);
        }

        // The strings are in gen1 after the first GC and leave it in the second, which merges them. The gen2 GCs reclaim the duplicates.
        GC.Collect(1, GCCollectionMode.Forced, blocking: true, compacting: true);
        GC.Collect(1, GCCollectionMode.Forced, blocking: true, compacting: true);
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);

        VerifyContents(plain, Contents, "plain ");
        VerifyContents(hashCoded, 1, "plain ");

        // Counting gives the strings a hash code, so it comes after everything that depends on the GCs above.
        int objects = CountObjects(plain);
        Console.WriteLine($"{plain.Length} strings with {Contents} different contents are {objects} objects");
        Assert.True(objects <= plain.Length / 10, $"{plain.Length} strings with {Contents} different contents are {objects} objects: were they merged (DOTNET_GCStringDedupMerge=1)?");
        Assert.Equal(Copies, CountObjects(hashCoded));

        for (int i = 0; i < Copies; i++)
        {
            Assert.Equal(hashCodes[i], RuntimeHelpers.GetHashCode(hashCoded[i]));
        }
    }

    // The strings are built at run time, so none of them is interned.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static string[] CreateStrings(int contents, int copies, string prefix)
    {
        string[] strings = new string[contents * copies];
        for (int i = 0; i < strings.Length; i++)
        {
            strings[i] = prefix + (i % contents).ToString();
        }

        return strings;
    }

    private static void VerifyContents(string[] strings, int contents, string prefix)
    {
        for (int i = 0; i < strings.Length; i++)
        {
            Assert.Equal(prefix + (i % contents).ToString(), strings[i]);
        }
    }

    private static int CountObjects(string[] strings)
    {
        HashSet<object> objects = new HashSet<object>(ReferenceEqualityComparer.Instance);
        foreach (string s in strings)
        {
            objects.Add(s);
        }

        return objects.Count;
    }
}
