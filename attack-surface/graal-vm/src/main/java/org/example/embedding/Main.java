/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * The Universal Permissive License (UPL), Version 1.0
 *
 * Subject to the condition set forth below, permission is hereby granted to any
 * person obtaining a copy of this software, associated documentation and/or
 * data (collectively the "Software"), free of charge and under any and all
 * copyright rights in the Software, and any and all patent rights owned or
 * freely licensable by each licensor hereunder covering either (i) the
 * unmodified Software as contributed to or provided by such licensor, or (ii)
 * the Larger Works (as defined below), to deal in both
 *
 * (a) the Software, and
 *
 * (b) any piece of software and/or hardware listed in the lrgrwrks.txt file if
 * one is included with the Software each a "Larger Work" to which the Software
 * is contributed by such licensors),
 *
 * without restriction, including without limitation the rights to copy, create
 * derivative works of, display, perform, and distribute the Software and make,
 * use, sell, offer for sale, import, export, have made, and have sold the
 * Software and the Larger Work(s), and to sublicense the foregoing rights on
 * either these or other terms.
 *
 * This license is subject to the following condition:
 *
 * The above copyright notice and either this complete permission notice or at a
 * minimum a reference to the UPL must be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Branch Target Reuse (BTR)
 * April 30th 2026
 * Yuhui Zhu
 */

package org.example.embedding;

import org.graalvm.polyglot.Context;
import org.graalvm.polyglot.Source;
import org.graalvm.polyglot.Value;
import org.graalvm.polyglot.SandboxPolicy;
import org.graalvm.polyglot.HostAccess;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;

import java.io.IOException;
import java.io.File;
import java.nio.file.Files;
import java.util.List;
import java.util.Set;
import java.util.concurrent.TimeUnit;

/**
 * A basic polyglot application that tries to exercise a simple hello world style program in all installed languages.
 */
public class Main {

    public static void main(String[] args) throws IOException {
        int nr_targets = 0;
        int length_target = 0;
        int repeats = 10;

        for (int i = 0; i < args.length; i++) {
            if ((args[i].equals("--repeats") || args[i].equals("-r")) && i + 1 < args.length) {
                repeats = Integer.parseInt(args[++i]);
            }
        }
        
        System.setProperty("graal.PrintAssembly", "true");

        ByteArrayOutputStream out1 = new ByteArrayOutputStream();

        try (Context context = Context.newBuilder("python").allowAllAccess(true)
        // .out(out1)
        // .err(new ByteArrayOutputStream())
        .option("engine.SpawnIsolate", "true")
        .option("engine.UntrustedCodeMitigation", "software")
        // .option("engine.MaxIsolateMemory", "256MB")
        // .option("sandbox.MaxCPUTime", "2s")
        .option("compiler.TraceInlining", "true")
        .option("engine.TraceCompilation", "true")
        .build()) {

        // try (Context context = Context.newBuilder("python")
        // .sandbox(SandboxPolicy.UNTRUSTED)
        // .out(out1)
        // .err(new ByteArrayOutputStream())
        // .option("engine.SpawnIsolate", "true")
        // .option("engine.UntrustedCodeMitigation", "software")
        // .allowHostAccess(HostAccess.UNTRUSTED)
        // .option("engine.MaxIsolateMemory", "1024MB")
        // .option("sandbox.MaxHeapMemory", "128MB")
        // .option("sandbox.MaxThreads","10")
        // .option("sandbox.MaxASTDepth","10")
        // .option("sandbox.MaxOutputStreamSize","64B")
        // .option("sandbox.MaxErrorStreamSize","0B")
        // .option("sandbox.MaxCPUTime", "100s")
        // // .option("compiler.TraceInlining", "true")
        // // .option("engine.TraceCompilation", "true")
        // .build()) {


            // System.out.println("Start!");

            // for (int i = 0; i < 4000; i++) {
            //     context.eval("python", "x = 5 * 60");
            // }

            // System.out.println("Next!");

            // for (int i = 0; i < 4000; i++) {
            //     context.eval("python", "x = 5 * 60");
            // }

            File scriptFile = new File("src/main/resources/vm.py");

            // Forward Java args to Python sys.argv before loading the script
            Value setArgv = context.eval("python",
                "import sys\ndef __set_argv(a): sys.argv = ['" + scriptFile.getPath() + "'] + list(a)\n__set_argv");
            setArgv.execute((Object) args);

            context.eval(Source.newBuilder("python", scriptFile).build());

            Value pythonMain = context.getBindings("python").getMember("main_loop");
            int warmUpIterations = 10000;

            // Run it - after a few thousand iterations, Graal will have
            // inlined: sub_compute -> main_loop -> (possibly) Java call site
            // double result = pythonMain.execute(largeData, warmUpIterations).asDouble();
            System.out.println("Executing python...");
            long hit0 = 0, hit1 = 0;
            for (int i = 0; i < repeats; i++) {
                Value ret = pythonMain.execute();
                hit0 += ret.getArrayElement(0).asLong();
                hit1 += ret.getArrayElement(1).asLong();
            }
            // Value result = pythonMain.execute(warmUpIterations);
            System.out.println("hit0: " + hit0/repeats + ", hit1: " + hit1/repeats);
            // System.out.println("Captured Output:\n" + out1.toString("UTF-8"));
            System.exit(0);
        }

        // int global = 0;
        // int result = 0;
        // for (int i = 0; i < 1000000000; i++) {
        //     result = workload(global, 4343);

        //     if (result % 99 == 0) {
        //         result += workload(result, 5454);
        //     }
        //     global = global + result;
        // }

        // System.out.println("End result: " + global);

    }

    // private static int workload(int a, int b) {
    //     return a + b;
    // }
}
